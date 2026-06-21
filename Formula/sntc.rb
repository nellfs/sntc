class Sntc < Formula
  desc "Simple network chat — LAN chat over TCP"
  homepage "https://github.com/nellfs/sntc"
  url "https://github.com/nellfs/sntc/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "7249ebc42fe0ada824f8b621ab83e15e5f6d6f03fad73c28616be915c6d58780"
  license "GPL-3.0-or-later"

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_match "usage:", shell_output("#{bin}/sntc 2>&1", 1)
  end
end
