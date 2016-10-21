#include "GameRenderer.hpp"

#include "World.hpp"

#include <epoxy/gl.h>

#include <radix/renderer/Renderer.hpp>
#include <radix/renderer/RenderContext.hpp>
#include <radix/World.hpp>

#include <radix/Viewport.hpp>
#include <radix/Camera.hpp>
#include <radix/Entity.hpp>

#include <radix/component/ViewFrame.hpp>
#include <radix/component/MeshDrawable.hpp>

#include <radix/shader/ShaderLoader.hpp>
#include <radix/model/MeshLoader.hpp>
#include <radix/material/MaterialLoader.hpp>

using namespace radix;

namespace glPortal {

GameRenderer::GameRenderer(glPortal::World& w, radix::Renderer& ren) :
  world(w),
  renderer(ren),
  camera(nullptr),
  viewportWidth(0), viewportHeight(0) {
  rc = std::make_unique<RenderContext>(ren);
}

void GameRenderer::render(double dtime, const Camera &cam) {
  time += dtime;
  renderer.getViewport()->getSize(&viewportWidth, &viewportHeight);

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);

  glClearColor(0, 0, 0, 1.0);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  Camera camera = cam;
  camera.setPerspective();
  camera.setAspect((float) viewportWidth / viewportHeight);

  rc->projStack.resize(1);
  camera.getProjMatrix(rc->projStack.back());

  rc->viewStack.resize(1);
  camera.getViewMatrix(rc->viewStack.back());

  rc->invViewStack.resize(1);
  camera.getInvViewMatrix(rc->invViewStack.back());

  rc->viewFramesStack.clear();

  rc->projDirty = rc->viewDirty = true;

  renderScene(*rc);
}

void GameRenderer::renderScene(RenderContext &rc) {
  if (rc.viewFramesStack.size() > rc.viewStackMaxDepth) {
    return;
  }
  RectangleI scissor;
  if (rc.viewFramesStack.size() > 0) {
    const RenderContext::ViewFrameInfo &vfi = rc.getViewFrame();
    // Don't render further if computed clip rect is zero-sized
    if (!renderer.clipViewFrame(rc, vfi.first, vfi.second, scissor)) {
      return;
    }
  }

  glClear(GL_DEPTH_BUFFER_BIT);

  renderViewFrames(rc);

  if (rc.viewFramesStack.size() > 0) {
    glScissor(scissor.x, scissor.y, scissor.w, scissor.h);
    renderViewFrameStencil(rc);
  }

  renderEntities(rc);
}

void GameRenderer::renderViewFrames(RenderContext &rc) {
  GLboolean save_stencil_test;
  glGetBooleanv(GL_STENCIL_TEST, &save_stencil_test);
  GLboolean save_scissor_test;
  glGetBooleanv(GL_SCISSOR_TEST, &save_scissor_test);

  glEnable(GL_STENCIL_TEST);
  glEnable(GL_SCISSOR_TEST);
  for (Entity &e : world.entities) {
    if (e.hasComponent<ViewFrame>()) {
      const Transform &t = e.getComponent<Transform>();
      Matrix4f inMat; t.getModelMtx(inMat);
      const ViewFrame &vf = e.getComponent<ViewFrame>();
      Matrix4f outMat;
      outMat.translate(vf.view.getPosition());
      outMat.rotate(vf.view.getOrientation());
      Matrix4f frameView = renderer.getFrameView(rc.getView(), inMat, outMat);
      rc.pushViewFrame(RenderContext::ViewFrameInfo(vf.mesh, t));
      rc.pushView(frameView);
      renderScene(rc);
      rc.popView();
      rc.popViewFrame();
    }
  }
  if (!save_stencil_test) {
    glDisable(GL_STENCIL_TEST);
  }
  if (!save_scissor_test) {
    glDisable(GL_SCISSOR_TEST);
  }

  // Draw portal in the depth buffer so they are not overwritten
  glClear(GL_DEPTH_BUFFER_BIT);

  GLboolean save_color_mask[4];
  GLboolean save_depth_mask;
  glGetBooleanv(GL_COLOR_WRITEMASK, save_color_mask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &save_depth_mask);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_TRUE);
  Shader shader = ShaderLoader::getShader("whitefill.frag");
  Matrix4f modelMtx;
  for (size_t i = 0; i < rc.viewFramesStack.size(); i++) {
    renderer.renderMesh(rc, shader, modelMtx, rc.viewFramesStack[i].first, nullptr);
  }
  glColorMask(save_color_mask[0], save_color_mask[1], save_color_mask[2], save_color_mask[3]);
  glDepthMask(save_depth_mask);
}

void GameRenderer::renderViewFrameStencil(RenderContext &rc) {
  GLboolean save_color_mask[4];
  GLboolean save_depth_mask;
  glGetBooleanv(GL_COLOR_WRITEMASK, save_color_mask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &save_depth_mask);

  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_FALSE);
  glStencilFunc(GL_NEVER, 0, 0xFF);
  glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);  // draw 1s on test fail (always)
  glClear(GL_STENCIL_BUFFER_BIT);  // needs mask=0xFF

  rc.pushView(rc.viewStack[0]);
  Shader shader = ShaderLoader::getShader("whitefill.frag");
  Matrix4f modelMtx; rc.viewFramesStack.back().second.getModelMtx(modelMtx);
  renderer.renderMesh(rc, shader, modelMtx, rc.viewFramesStack.back().first, nullptr);
  rc.popView();

  for (size_t i = 1; i < rc.viewStack.size() - 1; i++) {  // -1 to ignore last view
    // Increment intersection for current portal
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);  // draw 1s on test fail (always)
    renderer.renderMesh(rc, shader, modelMtx, rc.viewFramesStack.back().first, nullptr);

    // Decremental outer portal -> only sub-portal intersection remains
    glStencilFunc(GL_NEVER, 0, 0xFF);
    glStencilOp(GL_DECR, GL_KEEP, GL_KEEP);  // draw 1s on test fail (always)
    rc.pushView(rc.viewStack[i-1]);
    renderer.renderMesh(rc, shader, modelMtx, rc.viewFramesStack.back().first, nullptr);
  }

  //glColorMask(GL_TRUE, GL_TRUE, GL_FALSE, GL_TRUE);  // blue-ish filter if drawing on white or grey
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  /* Fill 1 or more */
  glStencilFunc(GL_LEQUAL, 1, 0xFF);
  glColorMask(save_color_mask[0], save_color_mask[1], save_color_mask[2], save_color_mask[3]);
  glDepthMask(save_depth_mask);
}

void GameRenderer::renderEntities(RenderContext &rc) {
  for (Entity &e : world.entities) {
    if (e.hasComponent<MeshDrawable>()) {
      renderEntity(rc, e);
    }
  }
}

void GameRenderer::renderEntity(RenderContext &rc, const Entity &e) {
  MeshDrawable &drawable = e.getComponent<MeshDrawable>();
  Matrix4f mtx;
  e.getComponent<Transform>().getModelMtx(mtx);

  if (drawable.material.fancyname.compare("Metal tiles .5x") == 0) {
    Shader &metal = ShaderLoader::getShader("metal.frag");
    renderer.renderMesh(rc, metal, mtx, drawable.mesh, drawable.material);
  } else {
    Shader &diffuse = ShaderLoader::getShader("diffuse.frag");
    renderer.renderMesh(rc, diffuse, mtx, drawable.mesh, drawable.material);
  }
}

void GameRenderer::renderPlayer(RenderContext &rc) {
  const Transform &t = world.getPlayer().getComponent<Transform>();
  Matrix4f mtx;
  mtx.translate(t.getPosition() + Vector3f(0, -.5f, 0));
  mtx.rotate(t.getOrientation());
  mtx.scale(Vector3f(1.3f, 1.3f, 1.3f));
  const Mesh &dummy = MeshLoader::getMesh("HumanToken.obj");
  const Material &mat = MaterialLoader::fromTexture("HumanToken.png");

  renderer.renderMesh(rc, ShaderLoader::getShader("diffuse.frag"), mtx, dummy, mat);
}

void GameRenderer::setCameraInPortal(const Camera &cam, Camera &dest,
                                 const Entity &portal, const Entity &otherPortal) {
  Transform &p1T = portal.getComponent<Transform>();
  Matrix4f p1mat;
  p1mat.translate(p1T.getPosition());
  p1mat.rotate(p1T.getOrientation());
  Transform &p2T = otherPortal.getComponent<Transform>();
  Matrix4f p2mat;
  p2mat.translate(p2T.getPosition());
  p2mat.rotate(p2T.getOrientation());
  Matrix4f rotate180; rotate180.rotate(rad(180), 0, 1, 0);
  Matrix4f view; cam.getViewMatrix(view);
  Matrix4f destView = view * p1mat * rotate180 * inverse(p2mat);

  dest.setPerspective();
  dest.setAspect(cam.getAspect());
  dest.setFovy(cam.getFovy());
  dest.setZNear((p1T.getPosition() - cam.getPosition()).length());
  dest.setViewMatrix(destView);
}

} /* namespace glPortal */
