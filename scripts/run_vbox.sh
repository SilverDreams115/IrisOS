#!/usr/bin/env bash
# Run IRIS in VirtualBox and bring both reports back out.
#
# There are two, and they answer different questions.  The SERIAL log is what
# the kernel said on its way up, including every test result.  `boot.rep` is
# what IRIS itself decided about the machine and wrote to its own partition --
# the half that works when nobody is watching, and the half that proves the
# filesystem survived the power going off.
#
# Both land as files on the Windows side.  Nothing here needs a person at the
# window, and nothing needs me.
#
#   scripts/run_vbox.sh            reuse the data disk (generation keeps rising)
#   scripts/run_vbox.sh --fresh    start from a pristine data disk
#   IRIS_VBOX_NIC=virtio ...       present a different card to the driver
#
# WHICH card, because the network service has a backend per family and this is
# a SECOND hypervisor: VirtualBox's own EFI lays the PCI windows out
# differently from OVMF, so a backend that works under QEMU has not yet been
# shown to work anywhere else.  `82540EM` (the default here) is the e1000 and
# `virtio` is virtio-net.  There is no Realtek option -- VirtualBox offers
# PCnet, Intel and virtio and nothing else -- which is why the r8169 backend
# is still unverified and only the real machine can answer for it.
set -u
VBM="/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
WIN='C:\Users\aethe\IRIS-VM'
DIR=/mnt/c/Users/aethe/IRIS-VM
VM=IRIS
FRESH=0
[ "${1:-}" = "--fresh" ] && FRESH=1

command -v python3 >/dev/null || { echo "python3 missing"; exit 1; }
[ -x "$VBM" ] || { echo "VBoxManage not at $VBM"; exit 1; }

echo "[vbox] building the boot image from build/efi_root"
python3 scripts/mkbootdisk.py || exit 1

"$VBM" controlvm "$VM" poweroff >/dev/null 2>&1
for i in $(seq 1 20); do "$VBM" list runningvms | grep -q "\"$VM\"" || break; sleep 1; done

swap() {   # $1 raw source, $2 vdi basename, $3 port
    "$VBM" storageattach "$VM" --storagectl SATA --port "$3" --device 0 --medium none >/dev/null 2>&1
    "$VBM" closemedium disk "$WIN\\$2.vdi" --delete >/dev/null 2>&1
    rm -f "$DIR/$2.vdi"
    "$VBM" convertfromraw "$1" "$WIN\\$2.vdi" --format VDI >/dev/null 2>&1 || return 1
    "$VBM" storageattach "$VM" --storagectl SATA --port "$3" --device 0 \
           --type hdd --medium "$WIN\\$2.vdi" >/dev/null 2>&1
}

cp build/iris-boot.img "$DIR/iris-boot.img"
swap "$WIN\\iris-boot.img" iris-boot 0 || { echo "[vbox] boot disk swap failed"; exit 1; }

if [ "$FRESH" = 1 ]; then
    echo "[vbox] fresh data disk"
    cp build/iris-disk.img "$DIR/iris-disk.img"
    swap "$WIN\\iris-disk.img" iris-data 1 || { echo "[vbox] data disk swap failed"; exit 1; }
fi

NIC="${IRIS_VBOX_NIC:-82540EM}"
echo "[vbox] network card: $NIC"
"$VBM" modifyvm "$VM" --nictype1 "$NIC" >/dev/null 2>&1 || {
    echo "[vbox] VirtualBox will not present a '$NIC'"; exit 1; }

rm -f "$DIR/iris-serial.log"
echo "[vbox] starting $VM headless"
"$VBM" startvm "$VM" --type headless >/dev/null || exit 1

# The suite takes about fifty seconds under QEMU on one processor and several
# times that here: VirtualBox runs it on two, and the SMP stress tests scale
# with the core count rather than against it.  A wait that ends early powers
# the machine off MID-SUITE, which then looks exactly like a hang and leaves
# the report on disk one boot stale -- so the budget is generous and a
# caller's own is honoured.
WAIT="${IRIS_VBOX_WAIT:-320}"
echo -n "[vbox] waiting for the suite"
for i in $(seq 1 "$WAIT"); do
    grep -aq "SUITE \(PASS\|FAIL\)" "$DIR/iris-serial.log" 2>/dev/null && break
    echo -n .; sleep 2
done
echo
if ! grep -aq "SUITE \(PASS\|FAIL\)" "$DIR/iris-serial.log" 2>/dev/null; then
    echo "[vbox] the suite did not finish inside ${WAIT} x 2s -- what follows is"
    echo "[vbox] a machine cut off mid-run, not a machine that failed."
fi
"$VBM" controlvm "$VM" poweroff >/dev/null 2>&1
sleep 2

echo
echo "── what the kernel said (serial) ──────────────────────────────"
tr -d '\r' < "$DIR/iris-serial.log" | grep -aE "SUITE|FAIL:|IRIS KERNEL|MMIO64|pci:|blk:|net:|ip:|fs:|processors online" | head -24

echo
echo "── what IRIS wrote to its own partition ───────────────────────"
# VirtualBox keeps a medium registered by path even after the file is gone, so
# the previous extraction has to be unregistered before a new one can take the
# same name.
"$VBM" closemedium disk "$WIN\\out.img" >/dev/null 2>&1
rm -f "$DIR/out.img"
if "$VBM" clonemedium disk "$WIN\\iris-data.vdi" "$WIN\\out.img" --format RAW >/dev/null 2>&1; then
    python3 scripts/readrep.py "$DIR/out.img" 2>&1 | head -16
else
    echo "  (could not extract the data disk)"
fi

echo
echo "[vbox] full log: $DIR/iris-serial.log"
