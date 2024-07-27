package storage

import (
	"FC/configs"
	"context"
	"encoding/json"
	"fmt"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"strconv"
)

// contention is too high: deprecated

type MongoDB struct {
	ctx    context.Context
	client *mongo.Client
	main   *mongo.Collection
}

type YCSBDataMongo struct {
	Key             string   `json:"key" bson:"_id"`
	Value           *RowData `json:"value" bson:"value"`
	WriteLatchOwner uint32   `json:"writeLatchOwner" bson:"writeLatchOwner"`
	ReadCnt         int      `json:"readCnt" bson:"readCnt"`
	ReadLatchOwners []uint32 `json:"readLatchOwners" bson:"readLatchOwners"`
	OldValue        *RowData `bson:"oldValue" bson:"oldValue"` // for rollback.
}

func (c *YCSBDataMongo) String() string {
	byt, _ := json.Marshal(c)
	return string(byt)
}

func toString(c interface{}) string {
	byt, _ := json.Marshal(c)
	return string(byt)
}

func (c *MongoDB) init(name string) {
	var err error
	c.ctx = context.TODO()
	c.client, err = mongo.Connect(c.ctx, options.Client().ApplyURI(configs.MongoDBLink))
	if err != nil {
		panic(err)
	}
	err = c.client.Ping(c.ctx, readpref.Primary())
	if err != nil {
		panic(err)
	}
	err = c.client.Database(fmt.Sprintf("flexi%s", name)).Collection("YCSB").Drop(c.ctx)
	if err != nil {
		panic(err)
	}
	c.main = c.client.Database(fmt.Sprintf("flexi%s", name)).Collection("YCSB")
}

func (c *MongoDB) Insert(tableName string, key uint64, value *RowData) bool {
	rec := YCSBDataMongo{Key: strconv.FormatUint(key, 10), Value: value, WriteLatchOwner: 0,
		ReadCnt: 0, ReadLatchOwners: make([]uint32, 0), OldValue: nil}
	_, err := c.main.InsertOne(c.ctx, rec)
	return err == nil
}

// Update do not use this API with transaction.
func (c *MongoDB) Update(tableName string, key uint64, value *RowData) bool {
	id := strconv.FormatUint(key, 10)
	_, err := c.main.UpdateOne(c.ctx, bson.M{"_id": id},
		bson.M{"$set": bson.M{"value": value}})
	return err == nil
}

// Read do not use this API with transaction.
func (c *MongoDB) Read(tableName string, key uint64) (*RowData, bool) {
	id := strconv.FormatUint(key, 10)
	res := YCSBDataMongo{}
	err := c.main.FindOne(c.ctx, bson.D{{"_id", id}}).Decode(&res)
	if err != nil {
		configs.JPrint(key)
		configs.JPrint(res)
		panic(err)
	}
	return res.Value, err == nil
}

func (c *MongoDB) forceUpdate(tableName string, key uint64, value *RowData) {
	for !c.Update(tableName, key, value) {
	}
}

func (c *MongoDB) forceRead(tableName string, key uint64) (*RowData, bool) {
	var data *RowData
	var ok bool = false
	for !ok {
		data, ok = c.Read(tableName, key)
	}
	return data, ok
}

func (c *MongoDB) updateWithRollback(tableName string, key uint64, value *RowData, oldValue *RowData) bool {
	id := strconv.FormatUint(key, 10)
	_, err := c.main.UpdateByID(c.ctx, id,
		bson.M{"$set": bson.M{"value": value, "oldValue": oldValue}})
	return err == nil
}

func (c *MongoDB) rollBack(tableName string, key uint64, oldValue *RowData) bool {
	id := strconv.FormatUint(key, 10)
	_, err := c.main.UpdateByID(c.ctx, id,
		bson.M{"$set": bson.M{"value": oldValue}})
	return err == nil
}
