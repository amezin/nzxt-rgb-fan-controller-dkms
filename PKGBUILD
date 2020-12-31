pkgname=nzxt_grid-dkms
pkgver=0.1
pkgrel=1
pkgdesc='Kernel driver for NZXT Grid V3 fan controller'
url=
arch=(any)
license=(GPL)
depends=(dkms)
source=(
  Makefile
  Kbuild
  nzxt_grid.c
  dkms.conf
)
md5sums=('SKIP' 'SKIP' 'SKIP' 'SKIP')

pkgver() {
  echo $(source dkms.conf && echo ${PACKAGE_VERSION})
}

package() {
  install -Dm 644 Makefile Kbuild nzxt_grid.c dkms.conf -t "${pkgdir}"/usr/src/nzxt_grid-${pkgver}
}
