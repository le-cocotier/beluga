# Homebrew formula for beluga. Lives in the tap repository
# git.giorgio-nas.fr/arthur/homebrew-beluga as Formula/beluga.rb:
#
#   brew tap arthur/beluga https://git.giorgio-nas.fr/arthur/homebrew-beluga.git
#   brew install beluga
#
# packaging/render.sh VERSION fills in url and sha256 for a release.
class Beluga < Formula
  desc "Terminal text editor configured in Lisp, with LSP and a debugger"
  homepage "https://git.giorgio-nas.fr/arthur/beluga"
  url "https://git.giorgio-nas.fr/arthur/beluga/releases/download/v@VERSION@/beluga-@VERSION@.tar.gz"
  sha256 "@SHA256_SOURCE@"
  license "MIT"
  head "https://git.giorgio-nas.fr/arthur/beluga.git", branch: "main"

  depends_on "meson" => :build
  depends_on "ninja" => :build

  def install
    system "meson", "setup", "build", *std_meson_args
    system "meson", "compile", "-C", "build", "--verbose"
    system "meson", "install", "-C", "build"
  end

  def caveats
    <<~EOS
      The first start sets up ~/.beluga: the default config and
      ~/.beluga/packages/personal-config/init.lisp for your own settings.
      The bundled packages stay in #{opt_pkgshare}/packages and upgrade
      with beluga.
    EOS
  end

  test do
    assert_match "Beluga", shell_output("#{bin}/beluga --version")
    assert_predicate pkgshare/"config/init.lisp", :exist?
    ENV["HOME"] = testpath
    system bin/"beluga", "--sync-packages"
    assert_predicate testpath/".beluga/config/init.lisp", :exist?
  end
end
