// mk9 - Mortal Kombat Komplete Edition (Xbox 360), recompiled with ReXGlue.
//
// The app layer: where the game's files and saves live, the GPU plugin, and the hooks the
// launcher link and netplay need (mk9/frame.h, mk9/input.h).

#pragma once

#include <rex/image_info.h>
#include <rex/rex_app.h>

// the recompiled image (generated/default/mk9_init.cpp)
extern const rex::PPCImageInfo PPCImageConfig;

class Mk9App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Mk9App>(new Mk9App(ctx, "mk9", PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnShutdown() override;
};
