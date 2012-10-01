ncpus=2
[ ! -z "$1" ] && ncpus=$1
[ $ncpus -eq 4 -o $ncpus -eq 2 ] || ncpus=2

echo "Testing ncpus=$ncpus"

trcat()
{
    cat /sys/kernel/debug/tracing/trace
}

if lsmod | grep memsched; then
	rmmod memsched
fi

grep i5 /proc/cpuinfo && speed=18 || speed=29

if [ $ncpus -eq 4 ]; then
    echo "enable all cores "
    echo 1 > /sys/devices/system/cpu/cpu1/online
    echo 1 > /sys/devices/system/cpu/cpu2/online
    echo 1 > /sys/devices/system/cpu/cpu3/online
else
    echo "hotplug cpu 1 3"
    echo 0 > /sys/devices/system/cpu/cpu1/online
    echo 1 > /sys/devices/system/cpu/cpu2/online
    echo 0 > /sys/devices/system/cpu/cpu3/online
fi
thld_pct=90
for thld_us in 100; do
for aggr_pct in 100; do
    filename="result-${ncpus}core-$thld_us-$aggr_pct.txt"
    # [ -f "$filename" ] && mv $filename $filename.bak
    [ -f "${filename%.txt}-multi.trace" ] && mv "${filename%.txt}-multi.trace" "${filename%.txt}-multi.trace".bak
    [ -f "${filename%.txt}-solo.trace" ] && mv "${filename%.txt}-solo.trace" "${filename%.txt}-solo.trace".bak

for core2bw in 10 20 30 40 50 60 70 80 90; do
# for core2bw in 50 60 70 80 90; do
    core0bw=`expr \( 100 - $core2bw \) / \( $ncpus - 1 \)`
    echo "noreclaim core2bw=$core2bw core0bw=$core0bw"
    insmod memsched.ko g_period_us=1000 g_ns_per_event=$speed g_reclaim_threshold_pct=$thld_pct g_reclaim_threshold_us=$thld_us g_budget=$core0bw,$core0bw,$core2bw,$core0bw

    echo aggr $aggr_pct $aggr_pct 100 $aggr_pct > /sys/kernel/debug/memsched/limit
    echo "solo"
    ./bandwidth -t 1 -c 2 -p -19 -f $filename -l $core2bw
    {
	echo ">>> BW: $core2bw "
	cat /sys/kernel/debug/memsched/failcnt
	cat /sys/kernel/debug/memsched/usage
    }  >> ${filename%.txt}-solo.stat
    {
	echo 
	echo ">>>> BW: $core2bw"
	trcat | tail -n 300
    } >> ${filename%.txt}-solo.trace
    sync

    echo "multi"
    if [ $ncpus -eq 4 ]; then
	./bandwidth -t 1000 -c 0 -p -19 &
	./bandwidth -t 1000 -c 1 -p -19 &
	./bandwidth -t 1000 -c 3 -p -19 &
    else
	./bandwidth -t 1000 -c 0 -p -19 &
    fi
    sleep 0.2
    echo ""> /sys/kernel/debug/memsched/failcnt
    ./bandwidth -t 1 -c 2 -p -19 -f $filename -l $core2bw
    {
	echo ">>> BW: $core2bw "
	cat /sys/kernel/debug/memsched/failcnt
	cat /sys/kernel/debug/memsched/usage
    }  >> ${filename%.txt}-multi.stat
    {
	echo 
	echo ">>>> BW: $core2bw"
	trcat | tail -n 300 
    } >> ${filename%.txt}-multi.trace
    sync

    killall -9 bandwidth
    rmmod memsched
done # corebw
done # aggr pct
done # thld us

chown heechul.heechul result-*
