#include "io_wrapper/pangolin/pangolin_dso_viewer.h"

#include <glog/logging.h>

#include "full_system/full_system.h"
#include "full_system/hessian_blocks/hessian_blocks.h"
#include "full_system/immature_point.h"
#include "io_wrapper/pangolin/keyframe_display.h"
#include "util/global_calib.h"
#include "util/settings.h"

namespace dso {
namespace IOWrap {

PangolinDSOViewer::PangolinDSOViewer(int w, int h, bool startRunThread) {
  this->w = w;
  this->h = h;
  running = true;

  {
    boost::unique_lock<boost::mutex> lk(openImagesMutex);
    internalVideoImg = new MinimalImageB3(w, h);
    internalKFImg = new MinimalImageB3(w, h);
    internalResImg = new MinimalImageB3(w, h);
    videoImgChanged = kfImgChanged = resImgChanged = true;

    internalVideoImg->setBlack();
    internalKFImg->setBlack();
    internalResImg->setBlack();
  }

  { currentCam = new KeyFrameDisplay(); }

  needReset = false;

  if (startRunThread) {
    runThread = boost::thread(&PangolinDSOViewer::run, this);
  }
}

PangolinDSOViewer::~PangolinDSOViewer() {
  close();
  runThread.join();
}

void PangolinDSOViewer::run() {
  LOG(INFO) << "START PANGOLIN!";

  pangolin::CreateWindowAndBind("Main", 2 * w, 2 * h);
  const int UI_WIDTH = 180;

  glEnable(GL_DEPTH_TEST);

  // 3D visualization
  pangolin::OpenGlRenderState Visualization3D_camera(
      pangolin::ProjectionMatrix(w, h, 400, 400, w / 2, h / 2, 0.1, 1000),
      pangolin::ModelViewLookAt(-0, -5, -10, 0, 0, 0, pangolin::AxisNegY));

  pangolin::View& Visualization3D_display =
      pangolin::CreateDisplay()
          .SetBounds(0.0, 1.0, pangolin::Attach::Pix(UI_WIDTH), 1.0,
                     -w / (float)h)
          .SetHandler(new pangolin::Handler3D(Visualization3D_camera));

  // 3 images
  pangolin::View& d_kfDepth =
      pangolin::Display("imgKFDepth").SetAspect(w / (float)h);

  pangolin::View& d_video =
      pangolin::Display("imgVideo").SetAspect(w / (float)h);

  pangolin::View& d_residual =
      pangolin::Display("imgResidual").SetAspect(w / (float)h);

  pangolin::GlTexture texKFDepth(w, h, GL_RGB, false, 0, GL_RGB,
                                 GL_UNSIGNED_BYTE);
  pangolin::GlTexture texVideo(w, h, GL_RGB, false, 0, GL_RGB,
                               GL_UNSIGNED_BYTE);
  pangolin::GlTexture texResidual(w, h, GL_RGB, false, 0, GL_RGB,
                                  GL_UNSIGNED_BYTE);

  pangolin::CreateDisplay()
      .SetBounds(0.0, 0.3, pangolin::Attach::Pix(UI_WIDTH), 1.0)
      .SetLayout(pangolin::LayoutEqual)
      .AddDisplay(d_kfDepth)
      .AddDisplay(d_video)
      .AddDisplay(d_residual);

  // parameter reconfigure gui
  pangolin::CreatePanel("ui").SetBounds(0.0, 1.0, 0.0,
                                        pangolin::Attach::Pix(UI_WIDTH));

  pangolin::Var<int> settings_pointCloudMode("ui.PC_mode", 1, 1, 4, false);

  pangolin::Var<bool> settings_showKFCameras("ui.KFCam", false, true);
  pangolin::Var<bool> settings_showCurrentCamera("ui.CurrCam", true, true);
  pangolin::Var<bool> settings_showTrajectory("ui.Trajectory", true, true);
  pangolin::Var<bool> settings_showFullTrajectory("ui.FullTrajectory", false,
                                                  true);
  pangolin::Var<bool> settings_showActiveConstraints("ui.ActiveConst", true,
                                                     true);
  pangolin::Var<bool> settings_showAllConstraints("ui.AllConst", false, true);

  pangolin::Var<bool> settings_show3D("ui.show3D", true, true);
  pangolin::Var<bool> settings_showLiveDepth("ui.showDepth", true, true);
  pangolin::Var<bool> settings_showLiveVideo("ui.showVideo", true, true);
  pangolin::Var<bool> settings_showLiveResidual("ui.showResidual", false, true);

  pangolin::Var<bool> settings_showFramesWindow("ui.showFramesWindow", false,
                                                true);
  pangolin::Var<bool> settings_showFullTracking("ui.showFullTracking", false,
                                                true);
  pangolin::Var<bool> settings_showCoarseTracking("ui.showCoarseTracking",
                                                  false, true);

  pangolin::Var<int> settings_sparsity("ui.sparsity", 1, 1, 20, false);
  pangolin::Var<double> settings_scaledVarTH("ui.relVarTH", 0.001, 1e-10, 1e10,
                                             true);
  pangolin::Var<double> settings_absVarTH("ui.absVarTH", 0.001, 1e-10, 1e10,
                                          true);
  pangolin::Var<double> settings_minRelBS("ui.minRelativeBS", 0.1, 0, 1, false);

  pangolin::Var<bool> settings_resetButton("ui.Reset", false, false);

  pangolin::Var<int> settings_nPts(
      "ui.activePoints", setting_desiredPointDensity, 50, 5000, false);
  pangolin::Var<int> settings_nCandidates(
      "ui.pointCandidates", setting_desiredImmatureDensity, 50, 5000, false);
  pangolin::Var<int> settings_nMaxFrames("ui.maxFrames", setting_maxFrames, 4,
                                         10, false);
  pangolin::Var<double> settings_kfFrequency(
      "ui.kfFrequency", setting_kfGlobalWeight, 0.1, 3, false);
  pangolin::Var<double> settings_gradHistAdd(
      "ui.minGradAdd", setting_minGradHistAdd, 0, 15, false);

  pangolin::Var<double> settings_trackFps("ui.Track fps", 0, 0, 0, false);
  pangolin::Var<double> settings_mapFps("ui.KF fps", 0, 0, 0, false);

  // Default hooks for exiting (Esc) and fullscreen (tab).
  while (!pangolin::ShouldQuit() && running) {
    // Clear entire screen
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (setting_render_display3D) {
      // Activate efficiently by object
      Visualization3D_display.Activate(Visualization3D_camera);
      boost::unique_lock<boost::mutex> lk3d(model3DMutex);
      // pangolin::glDrawColouredCube();
      int refreshed = 0;
      for (KeyFrameDisplay* fh : keyframes) {
        float blue[3] = {0, 0, 1};
        if (this->settings_showKFCameras) {
          fh->drawCam(1, blue, 0.1);
        }

        refreshed += (int)(fh->refreshPC(
            refreshed < 10, this->settings_scaledVarTH, this->settings_absVarTH,
            this->settings_pointCloudMode, this->settings_minRelBS,
            this->settings_sparsity));
        fh->drawPC(1);
      }
      if (this->settings_showCurrentCamera) {
        currentCam->drawCam(2, 0, 0.2);
      }
      drawConstraints();
      lk3d.unlock();
    }

    openImagesMutex.lock();
    if (videoImgChanged) {
      texVideo.Upload(internalVideoImg->data, GL_BGR, GL_UNSIGNED_BYTE);
    }
    if (kfImgChanged) {
      texKFDepth.Upload(internalKFImg->data, GL_BGR, GL_UNSIGNED_BYTE);
    }
    if (resImgChanged) {
      texResidual.Upload(internalResImg->data, GL_BGR, GL_UNSIGNED_BYTE);
    }
    videoImgChanged = kfImgChanged = resImgChanged = false;
    openImagesMutex.unlock();

    // update fps counters
    {
      openImagesMutex.lock();
      float sd = 0;
      for (float d : lastNMappingMs) {
        sd += d;
      }
      settings_mapFps = lastNMappingMs.size() * 1000.0f / sd;
      openImagesMutex.unlock();
    }
    {
      model3DMutex.lock();
      float sd = 0;
      for (float d : lastNTrackingMs) {
        sd += d;
      }
      settings_trackFps = lastNTrackingMs.size() * 1000.0f / sd;
      model3DMutex.unlock();
    }

    if (setting_render_displayVideo) {
      d_video.Activate();
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      texVideo.RenderToViewportFlipY();
    }

    if (setting_render_displayDepth) {
      d_kfDepth.Activate();
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      texKFDepth.RenderToViewportFlipY();
    }

    if (setting_render_displayResidual) {
      d_residual.Activate();
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      texResidual.RenderToViewportFlipY();
    }

    // update parameters
    this->settings_pointCloudMode = settings_pointCloudMode.Get();

    this->settings_showActiveConstraints = settings_showActiveConstraints.Get();
    this->settings_showAllConstraints = settings_showAllConstraints.Get();
    this->settings_showCurrentCamera = settings_showCurrentCamera.Get();
    this->settings_showKFCameras = settings_showKFCameras.Get();
    this->settings_showTrajectory = settings_showTrajectory.Get();
    this->settings_showFullTrajectory = settings_showFullTrajectory.Get();

    setting_render_display3D = settings_show3D.Get();
    setting_render_displayDepth = settings_showLiveDepth.Get();
    setting_render_displayVideo = settings_showLiveVideo.Get();
    setting_render_displayResidual = settings_showLiveResidual.Get();

    setting_render_renderWindowFrames = settings_showFramesWindow.Get();
    setting_render_plotTrackingFull = settings_showFullTracking.Get();
    setting_render_displayCoarseTrackingFull =
        settings_showCoarseTracking.Get();

    this->settings_absVarTH = settings_absVarTH.Get();
    this->settings_scaledVarTH = settings_scaledVarTH.Get();
    this->settings_minRelBS = settings_minRelBS.Get();
    this->settings_sparsity = settings_sparsity.Get();

    setting_desiredPointDensity = settings_nPts.Get();
    setting_desiredImmatureDensity = settings_nCandidates.Get();
    setting_maxFrames = settings_nMaxFrames.Get();
    setting_kfGlobalWeight = settings_kfFrequency.Get();
    setting_minGradHistAdd = settings_gradHistAdd.Get();

    if (settings_resetButton.Get()) {
      LOG(WARNING) << "RESET!";
      settings_resetButton.Reset();
      setting_fullResetRequested = true;
    }

    // Swap frames and Process Events
    pangolin::FinishFrame();

    if (needReset) {
      reset_internal();
    }
  }

  LOG(WARNING) << "QUIT Pangolin thread!";
  LOG(WARNING) << "I'll just kill the whole process. So Long, and Thanks for "
                  "All the Fish!";
  exit(1);
}

void PangolinDSOViewer::close() { running = false; }

void PangolinDSOViewer::join() {
  runThread.join();
  LOG(WARNING) << "JOINED Pangolin thread!";
}

void PangolinDSOViewer::reset() { needReset = true; }

void PangolinDSOViewer::reset_internal() {
  model3DMutex.lock();
  for (size_t i = 0; i < keyframes.size(); ++i) {
    delete keyframes[i];
  }
  keyframes.clear();
  allFramePoses.clear();
  keyframesByKFID.clear();
  connections.clear();
  model3DMutex.unlock();

  openImagesMutex.lock();
  internalVideoImg->setBlack();
  internalKFImg->setBlack();
  internalResImg->setBlack();
  videoImgChanged = kfImgChanged = resImgChanged = true;
  openImagesMutex.unlock();

  needReset = false;
}

void PangolinDSOViewer::drawConstraints() {
  if (settings_showAllConstraints) {
    // draw constraints
    glLineWidth(1);
    glBegin(GL_LINES);

    glColor3f(0, 1, 0);
    glBegin(GL_LINES);
    for (unsigned int i = 0; i < connections.size(); ++i) {
      if (connections[i].to == 0 || connections[i].from == 0) {
        continue;
      }
      int nAct = connections[i].bwdAct + connections[i].fwdAct;
      int nMarg = connections[i].bwdMarg + connections[i].fwdMarg;
      if (nAct == 0 && nMarg > 0) {
        Sophus::Vector3f t =
            connections[i].from->camToWorld.translation().cast<float>();
        glVertex3f((GLfloat)t[0], (GLfloat)t[1], (GLfloat)t[2]);
        t = connections[i].to->camToWorld.translation().cast<float>();
        glVertex3f((GLfloat)t[0], (GLfloat)t[1], (GLfloat)t[2]);
      }
    }
    glEnd();
  }

  if (settings_showActiveConstraints) {
    glLineWidth(3);
    glColor3f(0, 0, 1);
    glBegin(GL_LINES);
    for (unsigned int i = 0; i < connections.size(); ++i) {
      if (connections[i].to == 0 || connections[i].from == 0) {
        continue;
      }
      int nAct = connections[i].bwdAct + connections[i].fwdAct;

      if (nAct > 0) {
        Sophus::Vector3f t =
            connections[i].from->camToWorld.translation().cast<float>();
        glVertex3f((GLfloat)t[0], (GLfloat)t[1], (GLfloat)t[2]);
        t = connections[i].to->camToWorld.translation().cast<float>();
        glVertex3f((GLfloat)t[0], (GLfloat)t[1], (GLfloat)t[2]);
      }
    }
    glEnd();
  }

  if (settings_showTrajectory) {
    float colorRed[3] = {1, 0, 0};
    glColor3f(colorRed[0], colorRed[1], colorRed[2]);
    glLineWidth(3);

    glBegin(GL_LINE_STRIP);
    for (unsigned int i = 0; i < keyframes.size(); ++i) {
      glVertex3f((float)keyframes[i]->camToWorld.translation()[0],
                 (float)keyframes[i]->camToWorld.translation()[1],
                 (float)keyframes[i]->camToWorld.translation()[2]);
    }
    glEnd();
  }

  if (settings_showFullTrajectory) {
    float colorGreen[3] = {0, 1, 0};
    glColor3f(colorGreen[0], colorGreen[1], colorGreen[2]);
    glLineWidth(3);

    glBegin(GL_LINE_STRIP);
    for (unsigned int i = 0; i < allFramePoses.size(); ++i) {
      glVertex3f((float)allFramePoses[i][0], (float)allFramePoses[i][1],
                 (float)allFramePoses[i][2]);
    }
    glEnd();
  }
}

void PangolinDSOViewer::publishGraph(
    const std::map<
        uint64_t, Eigen::Vector2i, std::less<uint64_t>,
        Eigen::aligned_allocator<std::pair<const uint64_t, Eigen::Vector2i>>>&
        connectivity) {
  if (disableAllDisplay || !setting_render_display3D) {
    return;
  }

  model3DMutex.lock();
  connections.resize(connectivity.size());
  int runningID = 0;
  int totalActFwd = 0, totalActBwd = 0, totalMargFwd = 0, totalMargBwd = 0;
  for (std::pair<uint64_t, Eigen::Vector2i> p : connectivity) {
    int host = (int)(p.first >> 32);
    int target = (int)(p.first & (uint64_t)0xFFFFFFFF);

    CHECK_GE(host, 0);
    CHECK_GE(target, 0);
    if (host == target) {
      CHECK_EQ(p.second[0], 0);
      CHECK_EQ(p.second[1], 0);
      continue;
    }

    if (host > target) {
      continue;
    }

    connections[runningID].from =
        keyframesByKFID.count(host) == 0 ? 0 : keyframesByKFID[host];
    connections[runningID].to =
        keyframesByKFID.count(target) == 0 ? 0 : keyframesByKFID[target];
    connections[runningID].fwdAct = p.second[0];
    connections[runningID].fwdMarg = p.second[1];
    totalActFwd += p.second[0];
    totalMargFwd += p.second[1];

    uint64_t inverseKey = (((uint64_t)target) << 32) + ((uint64_t)host);
    Eigen::Vector2i st = connectivity.at(inverseKey);
    connections[runningID].bwdAct = st[0];
    connections[runningID].bwdMarg = st[1];

    totalActBwd += st[0];
    totalMargBwd += st[1];

    ++runningID;
  }

  model3DMutex.unlock();
}
void PangolinDSOViewer::publishKeyframes(std::vector<FrameHessian*>& frames,
                                         bool final, CalibHessian* HCalib) {
  if (disableAllDisplay || !setting_render_display3D) {
    return;
  }

  boost::unique_lock<boost::mutex> lk(model3DMutex);
  for (FrameHessian* fh : frames) {
    if (keyframesByKFID.find(fh->frameID) == keyframesByKFID.end()) {
      KeyFrameDisplay* kfd = new KeyFrameDisplay();
      keyframesByKFID[fh->frameID] = kfd;
      keyframes.emplace_back(kfd);
    }
    keyframesByKFID[fh->frameID]->setFromKF(fh, HCalib);
  }
}
void PangolinDSOViewer::publishCamPose(FrameShell* frame,
                                       CalibHessian* HCalib) {
  if (disableAllDisplay || !setting_render_display3D) {
    return;
  }

  boost::unique_lock<boost::mutex> lk(model3DMutex);
  struct timeval time_now;
  gettimeofday(&time_now, NULL);
  lastNTrackingMs.emplace_back(
      ((time_now.tv_sec - last_track.tv_sec) * 1000.0f +
       (time_now.tv_usec - last_track.tv_usec) / 1000.0f));
  if (lastNTrackingMs.size() > 10) {
    lastNTrackingMs.pop_front();
  }
  last_track = time_now;

  if (!setting_render_display3D) {
    return;
  }

  currentCam->setFromF(frame, HCalib);
  allFramePoses.emplace_back(frame->camToWorld.translation().cast<float>());
}

void PangolinDSOViewer::pushLiveFrame(FrameHessian* image) {
  if (disableAllDisplay || !setting_render_displayVideo) {
    return;
  }

  boost::unique_lock<boost::mutex> lk(openImagesMutex);

  for (int i = 0; i < w * h; ++i)
    internalVideoImg->data[i][0] = internalVideoImg->data[i][1] =
        internalVideoImg->data[i][2] =
            image->dI[i][0] * 0.8 > 255.0f ? 255.0 : image->dI[i][0] * 0.8;

  videoImgChanged = true;
}

bool PangolinDSOViewer::needPushDepthImage() {
  return setting_render_displayDepth;
}
void PangolinDSOViewer::pushDepthImage(MinimalImageB3* image) {
  if (disableAllDisplay || !setting_render_displayDepth) {
    return;
  }

  boost::unique_lock<boost::mutex> lk(openImagesMutex);

  struct timeval time_now;
  gettimeofday(&time_now, NULL);
  lastNMappingMs.emplace_back(
      ((time_now.tv_sec - last_map.tv_sec) * 1000.0f +
       (time_now.tv_usec - last_map.tv_usec) / 1000.0f));
  if (lastNMappingMs.size() > 10) {
    lastNMappingMs.pop_front();
  }
  last_map = time_now;

  memcpy((void*)internalKFImg->data, image->data, w * h * 3);
  kfImgChanged = true;
}
}
}
