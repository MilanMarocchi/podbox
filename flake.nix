{
  description = "PodBox — a standalone iPod manager for macOS";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # Darwin only, deliberately. PodBox's audio backend on anything else is a
      # no-op stub, and transcoding, the folder picker and the Apple Music
      # import all shell out to macOS tools. Adding Linux here would promise a
      # build that produces a program that does not work.
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            inherit system;
            pkgs = nixpkgs.legacyPackages.${system};
          }
        );
    in
    {
      packages = forAllSystems (
        { pkgs, ... }:
        rec {
          podbox = pkgs.callPackage ./nix/podbox.nix { };
          default = podbox;
        }
      );

      # `nix develop` gives you the toolchain plus the pinned dependency
      # sources, so an in-shell `cmake -S . -B build` configures offline and
      # builds exactly what `nix build` builds.
      devShells = forAllSystems (
        { pkgs, system }:
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.podbox ];
            packages = with pkgs; [
              cmake
              ninja
              clang-tools
            ];
            shellHook = ''
              echo "PodBox dev shell — cmake -S . -B build && cmake --build build -j"
            '';
          };
        }
      );

      checks = forAllSystems ({ system, ... }: { inherit (self.packages.${system}) podbox; });

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt);
    };
}
