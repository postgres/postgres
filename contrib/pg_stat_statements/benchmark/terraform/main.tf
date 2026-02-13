terraform {
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
    http = {
      source  = "hashicorp/http"
      version = "~> 3.0"
    }
    tls = {
      source  = "hashicorp/tls"
      version = "~> 4.0"
    }
    local = {
      source  = "hashicorp/local"
      version = "~> 2.0"
    }
  }
}

provider "aws" {
  region = var.region
}

# Auto-detect the caller's public IP if allowed_ssh_cidr is "auto"
data "http" "my_ip" {
  count = var.allowed_ssh_cidr == "auto" ? 1 : 0
  url   = "https://checkip.amazonaws.com"
}

locals {
  ssh_cidr = var.allowed_ssh_cidr == "auto" ? "${trimspace(data.http.my_ip[0].response_body)}/32" : var.allowed_ssh_cidr
}

# Generate an SSH key pair
resource "tls_private_key" "bench" {
  algorithm = "ED25519"
}

resource "aws_key_pair" "bench" {
  key_name_prefix = "pgss-bench-"
  public_key      = tls_private_key.bench.public_key_openssh
}

# Write the private key to a local file
resource "local_file" "ssh_key" {
  content         = tls_private_key.bench.private_key_openssh
  filename        = "${path.module}/pgss-bench-key.pem"
  file_permission = "0600"
}

# Use the default VPC
data "aws_vpc" "default" {
  default = true
}

# Find the latest Amazon Linux 2023 AMI
data "aws_ami" "al2023" {
  most_recent = true
  owners      = ["amazon"]

  filter {
    name   = "name"
    values = ["al2023-ami-*-x86_64"]
  }

  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
}

resource "aws_security_group" "pgss_bench" {
  name_prefix = "pgss-bench-"
  description = "SSH access for pg_stat_statements benchmark"
  vpc_id      = data.aws_vpc.default.id

  ingress {
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [local.ssh_cidr]
    description = "SSH"
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
    description = "All outbound"
  }

  tags = {
    Name = "pgss-cpu-time-benchmark"
  }
}

resource "aws_instance" "bench" {
  ami                    = data.aws_ami.al2023.id
  instance_type          = var.instance_type
  key_name               = aws_key_pair.bench.key_name
  vpc_security_group_ids = [aws_security_group.pgss_bench.id]

  root_block_device {
    volume_size = 30
    volume_type = "gp3"
  }

  user_data = <<-USERDATA
    #!/bin/bash
    set -euo pipefail

    # Install build dependencies for PostgreSQL
    dnf groupinstall -y "Development Tools"
    dnf install -y \
      readline-devel zlib-devel libxml2-devel openssl-devel \
      flex bison perl git rsync \
      stress-ng

    echo "=== Build dependencies installed ===" > /tmp/setup_done
  USERDATA

  tags = {
    Name = "pgss-cpu-time-benchmark"
  }
}
