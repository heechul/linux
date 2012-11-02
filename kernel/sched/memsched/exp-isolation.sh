#!/bin/bash

. functions

finish()
{
    sed 's/,//g' $outputfile | sed 's/ /,/g' > ${outputfile%.txt}.csv
#    sed 's/,//g' $outputfile | sed 's/ /,/g' > ${outputfile#.txt}.csv
    exit
}

#test_2core_twobench "470.lbm" "000.cpuhog" # (1) solor reference

init_system
rmmod memsched
set_cpus "1 1 1 1"
disable_prefetcher
set_cpus "1 0 1 0"


benchb=$allspec2006sorted #subject
outputfile=isolation.txt

test_isolation()
{
    target=$1
    do_init_share "1024 1024" 0 1200
    set_limit_mb "200 1000"
    bencha="000.none"
    benchb="$allspec2006sorted"
    do_exp 0 2

    bencha="470.lbm"
    do_exp 0 2

    set_limit_mb "800 1000"
    do_exp 0 2

    set_limit_mb "1400 1000"
    do_exp 0 2

    set_limit_mb "2000 1000"
    do_exp 0 2
}

test_isolation 
finish
