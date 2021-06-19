pkgname=nzxt_rgb_fan_controller-dkms
pkgver=0.1
pkgrel=1
pkgdesc='HWMON driver for NZXT RGB & Fan Controller'
url=https://github.com/amezin/nzxt-rgb-fan-controller-dkms
arch=(any)
license=(GPL)
depends=(dkms)
source=(
  Makefile
  Kbuild
  nzxt_rgb_fan_controller.c
  dkms.conf
)
md5sums=('SKIP' 'SKIP' 'SKIP' 'SKIP')

pkgver() {
  echo $(source dkms.conf && echo ${PACKAGE_VERSION})+g$(git rev-parse --short HEAD)
}

package() {
  install -Dm 644 Makefile Kbuild nzxt_rgb_fan_controller.c dkms.conf -t "${pkgdir}"/usr/src/nzxt_grid-${pkgver}
}
