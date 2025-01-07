#!/usr/bin/sysbench
-- This test is designed for testing MariaDB's key_cache_segments for MyISAM,
-- and should work with other storage engines as well.
--
-- For details about key_cache_segments please refer to:
-- http://kb.askmonty.org/v/segmented-key-cache
--

require("oltp_common")

-- Add random_points to the list of standard OLTP options
sysbench.cmdline.options.random_points =
   {"Number of random points in the IN() clause in generated SELECTs", 10}

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

   local points = string.rep("?, ", sysbench.opt.random_points - 1) .. "?"

   stmt = con:prepare(string.format([[
        SELECT id, k, c, pad
          FROM sbtest1
          WHERE k IN (%s)
        ]], points))

   params = {}
   for j = 1, sysbench.opt.random_points do
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
   for i = 1, sysbench.opt.random_points do
      local rmin = rlen * thread_id
      local rmax = rmin + rlen
      params[i]:set(sb_rand(rmin, rmax))
   end

   stmt:execute()
end
