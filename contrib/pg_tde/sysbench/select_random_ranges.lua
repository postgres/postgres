#!/usr/bin/sysbench
-- This test is designed for testing MariaDB's key_cache_segments for MyISAM,
-- and should work with other storage engines as well.
--
-- For details about key_cache_segments please refer to:
-- http://kb.askmonty.org/v/segmented-key-cache
--

require("oltp_common")

-- Add --number-of-ranges and --delta to the list of standard OLTP options
sysbench.cmdline.options.number_of_ranges =
   {"Number of random BETWEEN ranges per SELECT", 10}
sysbench.cmdline.options.delta =
   {"Size of BETWEEN ranges", 5}

-- Override standard prepare/cleanup OLTP functions, as this benchmark does not
-- support multiple tables
oltp_prepare = prepare
oltp_cleanup = cleanup

function prepare()
   assert(sysbench.opt.tables == 1, "this benchmark does not support " ..
             "--tables > 1")
   oltp_prepare()
end

function cleanup()
   assert(sysbench.opt.tables == 1, "this benchmark does not support " ..
             "--tables > 1")
   oltp_cleanup()
end

function thread_init()
   drv = sysbench.sql.driver()
   con = drv:connect()

   local ranges = string.rep("k BETWEEN ? AND ? OR ",
                             sysbench.opt.number_of_ranges - 1) ..
      "k BETWEEN ? AND ?"

   stmt = con:prepare(string.format([[
        SELECT count(k)
          FROM sbtest1
          WHERE %s]], ranges))

   params = {}
   for j = 1, sysbench.opt.number_of_ranges*2 do
      params[j] = stmt:bind_create(sysbench.sql.type.INT)
   end

   stmt:bind_param(unpack(params))

   rlen = sysbench.opt.table_size / sysbench.opt.threads

   thread_id = sysbench.tid % sysbench.opt.threads
end

function thread_done()
   stmt:close()
   con:disconnect()
end

function event()
   -- To prevent overlapping of our range queries we need to partition the whole
   -- table into 'threads' segments and then make each thread work with its
   -- own segment.
   for i = 1, sysbench.opt.number_of_ranges*2, 2 do
      local rmin = rlen * thread_id
      local rmax = rmin + rlen
      local val = sb_rand(rmin, rmax)
      params[i]:set(val)
      params[i+1]:set(val + sysbench.opt.delta)
   end

   stmt:execute()
end
