#!/usr/bin/perl

# Notes ... -B 1 == 8k

if(@ARGV > 1) {
  if($ARGV[0] eq "-B") {
    $buffers = $ARGV[1];
  }
}

if($buffers > 0) {
  $kb_memory_required = $buffers * 8;
}

$shm = `sysctl kern.ipc | grep shmall`;
( $junk, $shm_amt ) = split(/ /, $shm);
chomp($shm_amt);
$sem = `sysctl kern.ipc | grep semmap`;

print "\n\n";
if(length($shm) > 0) {
  printf "shared memory enabled: %d kB available\n", $shm_amt * 4;
  if($buffers > 0) {
    if($kb_memory_required / 4 > $shm_amt) {
      print "\n";
      print "to provide enough shared memory for a \"-B $buffers\" setting\n";
      print "issue the following command\n\n";
      printf "\tsysctl -w kern.ipc.shmall=%d\n", $kb_memory_required / 4;
      print "\nand add the following to your /etc/sysctl.conf file:\n\n";
      printf "\tkern.ipc.shmall=%d\n", $kb_memory_required / 4;
    } else {
      print "\n";
      print "no changes to kernel required for a \"-B $buffers\" setting\n";
    }
  }
} else {
  print "no shared memory support available\n";
  print "add the following option to your kernel config:\n\n";
  print "\toptions        SYSVSHM\n\n";
}

print "\n==========================\n\n";
if(length($sem) > 0) {
  print "semaphores enabled\n";
} else {
  print "no semaphore support available\n";
  print "add the following option to your kernel config:\n\n";
  print "\toptions        SYSVSEM\n\n";
}


