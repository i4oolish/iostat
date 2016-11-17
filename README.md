# iostat
iostat源码剖析

io: 利用/proc/diskstats计算
cpu: 利用/proc/stat计算

核心内容在于两个函数print_cpu_stats（）、print_partition_stats()中的计算部分。

---------------------------------------------------------

iostat - Linux I/O performance monitoring utility

Homepage: http://linux.inet.hr/

Check out the manual for more info.

-- 
Zlatko Calusic <zlatko@iskon.hr>, Jan  6 2004

