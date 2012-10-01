#/bin/bash

kilall -9 worst
taskset -c 0 ~/Projects/isolation/worst 1000000 &
taskset -c 1 ~/Projects/isolation/worst 1000000 &
taskset -c 2 ~/Projects/isolation/worst 1000000 &
taskset -c 3 ~/Projects/isolation/worst 1000000 &
