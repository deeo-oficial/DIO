# Maintainer: DEEO <davi.phantom07@gmail.com>
pkgname=diocli
pkgver=1.0.0
pkgrel=1
pkgdesc="Ultra-lightweight native desktop chat client for OpenRouter API written in C/GTK3"
arch=('x86_64')
url="https://github.com/deeo-oficial/dio"
license=('GPL3')
depends=('gtk3' 'curl' 'cjson')
makedepends=('git' 'gcc' 'pkg-config')
provides=('dio')
conflicts=('dio')
source=("git+https://github.com/deeo-oficial/dio.git")
md5sums=('SKIP')

build() {
  cd "$srcdir/dio"
  make
}

package() {
  cd "$srcdir/dio"
  install -Dm755 dio "$pkgdir/usr/bin/dio"
}
