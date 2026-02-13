output "instance_id" {
  value = aws_instance.bench.id
}

output "public_ip" {
  value = aws_instance.bench.public_ip
}

output "ssh_key_file" {
  description = "Path to the generated private key"
  value       = local_file.ssh_key.filename
}

output "ssh_command" {
  value = "ssh -i ${abspath(local_file.ssh_key.filename)} ec2-user@${aws_instance.bench.public_ip}"
}

output "rsync_command" {
  description = "Run this from the postgres repo root to push the source tree"
  value       = "rsync -avz -e 'ssh -i ${abspath(local_file.ssh_key.filename)}' --exclude='.git' --exclude='*.o' --exclude='*.dylib' . ec2-user@${aws_instance.bench.public_ip}:~/postgres/"
}

output "benchmark_command" {
  description = "Run this after SSH'ing in to start the benchmark"
  value       = "cd ~/postgres && bash contrib/pg_stat_statements/benchmark/setup.sh && bash contrib/pg_stat_statements/benchmark/run_benchmark.sh"
}
