#!/bin/bash

curl -X POST http://localhost:8080/api/cmd/stop -d '{}'
curl -X POST http://localhost:8080/api/cmd/terminate -d '{}'
