variable "instance_type" {
  description = "EC2 instance type"
  type        = string
  default     = "c5.2xlarge"
}

variable "region" {
  description = "AWS region"
  type        = string
  default     = "us-east-1"
}

variable "allowed_ssh_cidr" {
  description = "CIDR block allowed to SSH (e.g. your IP). Use 'auto' to detect."
  type        = string
  default     = "auto"
}
