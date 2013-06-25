#VER=3.6.6
#EXT=-custom-color
VER=3.6.0
EXT=-rc7-custom-color

do_sanity()
{
	chown -R heechul.heechul *
	chown -R heechul.heechul .*
	chown heechul.heechul .tmp_*
}

error()
{
	echo "ERROR: $*"
	do_sanity
	exit 1
}

do_kernel()
{
	make EXTRAVERSION=$EXT oldconfig || error "oldconfig"
	make EXTRAVERSION=$EXT -j4 bzImage || error "bzImage"
}
do_module()
{
	make EXTRAVERSION=$EXT oldconfig || error "oldconfig"
	make EXTRAVERSION=$EXT -j4 modules || error "module"
	make EXTRAVERSION=$EXT INSTALL_MOD_STRIP=1 modules_install || error "mod_install"
	do_sanity
}

do_update()
{
	cp .config /boot/config-$VER$EXT
	cp arch/x86/boot/bzImage /boot/vmlinuz-$VER$EXT
	do_sanity
	pushd /lib/modules/
	update-initramfs -ck $VER$EXT
	update-grub
}

do_kernel
do_module
do_update
