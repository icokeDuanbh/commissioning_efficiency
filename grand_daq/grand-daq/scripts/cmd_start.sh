#!/bin/bash

curl -X POST http://localhost:8080/api/cmd/initialize -d '{}'
curl -X POST http://localhost:8080/api/cmd/start -d '{}'
