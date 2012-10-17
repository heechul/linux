#!/bin/bash

. ./functions
PATH=.:$PATH

# r_min = 1.2GB/s
# w1:w2:w3:w4 = 1:2:4:8
# 1/15, 2/15, 4/15, 8/15 * 1.2GB
#  80M, 160M, 320M, 640MB

run_test()
{
    mode=$1
    sleep 1
    for i in `seq 3 -1 0`; do
	fps -c $i  > fps.log.C$i.$mode &
    done
    sleep 22
    killall -9 fps
    # produce gnuplot data
    process_log $mode
    plot $mode
}

process_log()
{
    mode=$1
    files=""
    for i in `seq 0 3`; do 
	grep fps fps.log.C$i.$mode | awk '{ print $2 }' > fps.dat.C$i.$mode
	files="$files fps.dat.C$i.$mode"
    done
    echo "files: $files"
    pr -tm $files > fps.dat.$mode
}

plot()
{
    # file msut be xxx.dat form
    mode=$1
    cat > fps.${mode}.scr <<EOF
set terminal postscript eps enhanced monochrome "Times-Roman" 40
set ylabel "Frames/sec."
set xlabel "Time(sec)"
set yrange [100:300]
set xrange [1:20]
plot 'fps.dat.C0.$mode' ti "Core0" w lp, \
    'fps.dat.C1.$mode' ti "Core1" w lp, \
    'fps.dat.C2.$mode' ti "Core2" w lp, \
    'fps.dat.C3.$mode' ti "Core3" w lp
EOF
    gnuplot fps.${mode}.scr > fps${mode}.eps
    epspdf  fps${mode}.eps
}


init_system
set_cpus "1 1 1 1"
disable_prefetcher

echo "*** Run w/o MemGuard ***"
run_test noregul

echo "*** Run with MemGuard ***"
do_init_share "1 2 4 8" 0 1200
set_exclusive_mode 2
run_test memguard
rmmod memsched
