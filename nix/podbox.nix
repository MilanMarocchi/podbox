# PodBox, built from pinned sources with no network access.
#
# Upstream's CMakeLists fetches GLFW, Dear ImGui, TagLib, stb and hashAB at configure
# time. Rather than restructure it, we hand FetchContent the sources it wants
# via FETCHCONTENT_SOURCE_DIR_<NAME> and switch it fully offline, so the plain
# `cmake -S . -B build` path keeps working unchanged for people without Nix
# while this one stays hermetic. Both build the same revisions.
{
  lib,
  stdenv,
  cmake,
  ninja,
  fetchFromGitHub,
  zlib,
  sqlite,
}:

let
  # Revisions match the GIT_TAGs in CMakeLists.txt. Keep the two in step: if
  # they drift, the Nix build and the plain CMake build stop being the same
  # program.
  sources = {
    glfw = fetchFromGitHub {
      owner = "glfw";
      repo = "glfw";
      rev = "7b6aead9fb88b3623e3b3725ebb42670cbe4c579"; # 3.4
      hash = "sha256-FcnQPDeNHgov1Z07gjFze0VMz2diOrpbKZCsI96ngz0=";
    };
    imgui = fetchFromGitHub {
      owner = "ocornut";
      repo = "imgui";
      rev = "f5befd2d29e66809cd1110a152e375a7f1981f06"; # v1.91.9b
      hash = "sha256-dkukDP0HD8CHC2ds0kmqy7KiGIh4148hMCyA1QF3IMo=";
    };
    taglib = fetchFromGitHub {
      owner = "taglib";
      repo = "taglib";
      rev = "c840222a391439285478820b4477d5fa6b78d63d"; # v1.13.1
      hash = "sha256-QX0EpHGT36UsgIfRf5iALnwxe0jjLpZvCTbk8vSMFF4=";
      # utf8-cpp is vendored in-tree, not a submodule, so nothing else to fetch.
    };
    stb = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "31c1ad37456438565541f4919958214b6e762fb4";
      hash = "sha256-m2yNUlA37hDkKQVrQ+R8nufHfW/cXLnMo+n1X1Cyun0=";
    };
    hashab = fetchFromGitHub {
      owner = "dstaley";
      repo = "hashab";
      rev = "cc3e8aac05172a52e32da259fb31ccca9c625218";
      hash = "sha256-opqgwxlWWe4uxW2ygclJ04by72TQd67MdxSMVjGVfrY=";
    };
  };

  # FetchContent uppercases the declared name to build the variable it looks up.
  sourceDirFlags = lib.mapAttrsToList (
    name: src: "-DFETCHCONTENT_SOURCE_DIR_${lib.toUpper name}=${src}"
  ) sources;
in
stdenv.mkDerivation {
  pname = "podbox";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../.;
    # Only the inputs the build actually reads, so editing the README or the
    # docs does not invalidate the build.
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../LICENSE  # CPack reads it for the DMG
      ../src
      ../tools
      ../packaging
    ];
  };

  # GLFW, OpenGL, AVFoundation and Cocoa come from the platform SDK. zlib
  # does not — iTunesCDB reads and writes a zlib stream, and nixpkgs' zlib
  # makes the same library available to CMake's find_package(ZLIB) that the
  # macOS SDK's libz is on the host.
  nativeBuildInputs = [
    cmake
    ninja
  ];
  buildInputs = [
    zlib
    sqlite
  ];

  cmakeFlags = sourceDirFlags ++ [
    # Belt and braces: with every source dir supplied there is nothing left to
    # fetch, and this turns a missing one into a configure error rather than a
    # silent network call that fails in the sandbox.
    (lib.cmakeBool "FETCHCONTENT_FULLY_DISCONNECTED" true)
    (lib.cmakeBool "PODBOX_INSTALL_TOOLS" true)
    # nixpkgs convention; the default is the DMG layout.
    (lib.cmakeFeature "PODBOX_BUNDLE_DEST" "Applications")
    # Release, not RelWithDebInfo, and the choice is load-bearing for
    # reproducibility. With -g, each object file records the directory it was
    # compiled in; Nix randomises that per build. The paths themselves are
    # stripped in fixup, but ld64 has already folded them into the binary's
    # LC_UUID, which survives — so two builds of identical source differ in
    # exactly those 16 bytes. Without -g there is nothing to differ.
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
  ];

  # FetchContent_MakeAvailable pulls TagLib's own install() rules into this
  # project, so `cmake --install` scatters its static lib, headers, pkg-config
  # file and taglib-config script into the output. PodBox links TagLib
  # statically and exposes none of it, so none of that belongs here. (GLFW is
  # already silenced upstream by GLFW_INSTALL=OFF; TagLib 1.13 has no
  # equivalent switch, and FetchContent's EXCLUDE_FROM_ALL needs CMake 3.28,
  # above the 3.24 the project targets.)
  postInstall = ''
    rm -rf $out/lib $out/include $out/bin/taglib-config

    # `nix run` looks for $out/bin/<mainProgram>, but the executable lives
    # inside the bundle. Launching it through this link still gives it the
    # bundle's identity — macOS resolves Info.plist from the enclosing
    # Contents/ — so the TCC usage strings it needs to talk to Music.app and
    # read removable volumes are honoured either way.
    ln -s ../Applications/PodBox.app/Contents/MacOS/PodBox $out/bin/PodBox
  '';

  doCheck = true;
  # Suites that assert rather than print, and that need neither a
  # mounted iPod nor a live Apple Music library. ctest knows them all, so this
  # cannot drift from the list the plain CMake build runs.
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';

  meta = {
    description = "Standalone iPod manager for macOS";
    longDescription = ''
      Manage the music library on a classic iPod the way the old iTunes did:
      drag songs on, delete songs off, build playlists — no full-library sync
      and no iTunes required. Reads and writes the iTunesDB directly.
    '';
    homepage = "https://github.com/MilanMarocchi/podbox";
    license = lib.licenses.mit;
    mainProgram = "PodBox";
    # macOS only. The Linux port is unfinished upstream: the audio backend is a
    # no-op stub and transcoding, the folder picker and the Apple Music import
    # all shell out to macOS tools.
    platforms = lib.platforms.darwin;
  };
}
