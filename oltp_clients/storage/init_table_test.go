package storage

import (
	"FC/configs"
	"context"
	"testing"
)

func TestTPCCStorageKit(t *testing.T) {
	configs.OuAddress = []string{"127.0.0.1:6001", "127.0.0.1:6002", "127.0.0.1:6003"}
	ctx := context.WithValue(context.Background(), "store", configs.BenchmarkStorage)
	_ = TPCCStorageKit(ctx, "127.0.0.1:5001")
}
