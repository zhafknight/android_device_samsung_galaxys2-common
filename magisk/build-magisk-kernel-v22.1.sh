#!/system/bin/sh
cout
if [[ ! -f ramdisk.cpio ]] ; then
    croot
    echo "Can't find ramdisk.cpio. Make sure LineageOS is fully build, aborting."
    return
fi

echo "*****************************************************************************************************" 
echo "* 1. Creating '/storage/emulated/0/Download/magisk_PATCH_THIS.img' on phone to be patched by Magisk *"
echo "*****************************************************************************************************" 
echo .>dummyKernel
mkbootimg --kernel dummyKernel --ramdisk ramdisk.cpio -o magisk_PATCH_THIS.img
rm dummyKernel
adb remount
adb push magisk_PATCH_THIS.img /storage/emulated/0/Download/magisk_PATCH_THIS.img
echo ""
echo "************************************************************************************************" 
echo "* 2. Manually patch '/storage/emulated/0/Download/magisk_PATCH_THIS.img' on phone using Magisk *"
echo "************************************************************************************************" 
echo "Please open Magisk (v22.1 or higher) on your phone and choose for 'Update' and method 'Patch a file'."
echo "Choose 'Download/magisk_PATCH_THIS.img' on your phone internal storage and patch it."
echo ""
read -p "Press [ENTER] when you have succesfully patched it with Magisk or CTRL+C to abort."
adb rm /storage/emulated/0/Download/magisk_PATCH_THIS.img

echo "*******************************************************************************************" 
echo "* 3. Rebuilding kernel and include Magisk patched ramdisk.cpio from magisk_PATCH_THIS.img *"
echo "*******************************************************************************************" 
echo "Extracting modified Magisk-ramdisk from magisk_patched.img..."
adb shell 'ls /storage/emulated/0/Download/magisk_patched-*.img' | tr -d '\r' | sed -e 's/^\///' | xargs -n1 adb pull
adb shell rm /storage/emulated/0/Download/magisk_patched-*.img
for f in magisk_patched-*.img; do mv "$f" "magisk_patched.img"; done

if [[ ! -f magisk_patched.img ]] ; then
    echo "You haven't patched magisk_PATCH_THIS.img or something failed during patching :(, aborting."
    return
fi
abootimg -x magisk_patched.img
rm ramdisk.cpio
rm bootimg.cfg
rm magisk_patched.img
rm zImage
mv initrd.img ramdisk.cpio

echo "Rebuilding kernel with Magisk modified ramdisk..."
croot
mka bootimage && cout
mv boot.img "magisk_patched-$(date "+%Y%m%d").img"
echo "Uploading '/storage/emulated/0/Download/magisk_patched-$(date "+%Y%m%d").img' to phone..."
adb push "magisk_patched-$(date "+%Y%m%d").img" /storage/emulated/0/Download/
echo ""
echo "*****************************************************************************************************" 
echo "***                                          DONE!!                                               ***" 
echo "***                                                                                               ***" 
echo "*** You can now reboot to TWRP and flash /storage/emulated/0/Download/magisk_patched-$(date "+%Y%m%d").img ***" 
echo "*****************************************************************************************************" 

