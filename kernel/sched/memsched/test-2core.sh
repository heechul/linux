
echo "hotplug cpu 1 3"
echo 0 > /sys/devices/system/cpu/cpu1/online
echo 1 > /sys/devices/system/cpu/cpu2/online
echo 0 > /sys/devices/system/cpu/cpu3/online

trcat()
{
    cat /sys/kernel/debug/tracing/trace
}

if lsmod | grep memsched; then
	rmmod memsched
fi

grep i5 /proc/cpuinfo && speed=18 || speed=29

thld_pct=90
for thld_us in 100; do
for aggr_pct in 20 100; do
    filename="result-2core-$thld_us-$aggr_pct.txt"
    # [ -f "$filename" ] && mv $filename $filename.bak
    [ -f "${filename%.txt}-multi.trace" ] && mv "${filename%.txt}-multi.trace" "${filename%.txt}-multi.trace".bak
    [ -f "${filename%.txt}-solo.trace" ] && mv "${filename%.txt}-solo.trace" "${filename%.txt}-solo.trace".bak

for core2bw in 10 20 30 40 50 60 70 80 90; do
# for core2bw in 80; do
    core0bw=`expr 100 - $core2bw`
    echo "noreclaim core2bw=$core2bw core0bw=$core0bw"
    insmod memsched.ko g_period_us=1000 g_ns_per_event=$speed g_reclaim_threshold_pct=$thld_pct g_reclaim_threshold_us=$thld_us g_budget=$core0bw,0,$core2bw,0

    echo aggr $aggr_pct 100 100 100 > /sys/kernel/debug/memsched/limit
    echo "solo"
    ./bandwidth -t 1 -c 2 -p -19 -f $filename -l $core2bw
    {
	echo 
	echo ">>>> BW: $core2bw"
	trcat | tail -n 300
    } >> ${filename%.txt}-solo.trace
    sync

    echo "multi"
    ./bandwidth -t 1000 -c 0 -p -19 &
    sleep 0.2
    ./bandwidth -t 1 -c 2 -p -19 -f $filename -l $core2bw
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
