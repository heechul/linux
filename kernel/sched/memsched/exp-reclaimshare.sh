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
outputfile=reclaimshare.txt

test_isolation()
{

    bencha="470.lbm"

    # reclaim
    do_init_mb "200 1000" 1
    do_exp 0 2

    # exclusice
    do_init_mb "200 1000" 0 2
    do_exp 0 2

    # reclaim+exclusice
    do_init_mb "200 1000" 1 2
    do_exp 0 2
}

test_isolation
finish
