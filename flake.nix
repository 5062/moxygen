{
  description = "moxygen relay with MoQ trace hooks";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # The system dependencies standalone/CMakeLists.txt expects to find. The
      # Meta libraries (folly, fizz, wangle, mvfst, proxygen) are fetched by
      # CMake at the revisions in build/deps/github_hashes, not taken from
      # nixpkgs, so they match the revisions upstream tests against.
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            git
            gperf
            boost
            c-ares
            double-conversion
            fmt
            gflags
            glog
            gtest
            libevent
            libsodium
            libunwind
            lttng-ust
            openssl
            xz
            zlib
            zstd
          ];
          hardeningDisable = [ "fortify" ];
        };
      });
    };
}
