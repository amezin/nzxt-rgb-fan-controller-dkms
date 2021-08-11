modname=nzxt-smart2
pkgname=${modname}-dkms
pkgver=0.1
pkgrel=1
pkgdesc='HWMON Driver for NZXT RGB & Fan Controller/Smart Device V2'
url=https://github.com/amezin/nzxt-rgb-fan-controller-dkms
arch=(any)
license=(GPL)
depends=(dkms)
provides=(nzxt_rgb_fan_controller-dkms nzxt-rgb-fan-controller-dkms)
conflicts=(nzxt_rgb_fan_controller-dkms nzxt-rgb-fan-controller-dkms)
source=(
  Makefile
  Kbuild
  ${modname}.c
  dkms.conf
)
md5sums=('SKIP' 'SKIP' 'SKIP' 'SKIP')

pkgver() {
  echo $(source dkms.conf && echo ${PACKAGE_VERSION})+g$(git rev-parse --short HEAD)
}

package() {
  install -Dm 644 ${source[@]} -t "${pkgdir}"/usr/src/${modname}-${pkgver}
}
