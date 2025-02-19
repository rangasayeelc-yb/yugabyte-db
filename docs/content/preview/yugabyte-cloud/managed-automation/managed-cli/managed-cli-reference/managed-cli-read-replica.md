---
title: cluster read-replica resource
headerTitle: ybm cluster read-replica
linkTitle: cluster read-replica
description: YugabyteDB Managed CLI reference Cluster Read Replica resource.
headcontent: Manage cluster read replicas
menu:
  preview_yugabyte-cloud:
    identifier: managed-cli-read-replica
    parent: managed-cli-reference
    weight: 20
type: docs
---

Use the `cluster read-replica` resource to perform operations on a YugabyteDB Managed cluster [read replica](../../../../cloud-clusters/managed-read-replica/), including the following:

- create, update, and delete read replicas
- get information about read replicas

## Syntax

```text
Usage: ybm cluster read-replica [command] [flags]
```

## Example

Create a read-replica cluster:

```sh
ybm cluster read-replica create \
  --replica=num-cores=2,\
  memory-mb=4096,\
  disk-size-gb=200,\
  code=<GCP or AWS>,\
  region=<region>,\
  num-nodes=3,\
  vpc=<vpc_name>,\
  num-replicas=<num_replicas>,\
  multi-zone=<bool>
```

## Commands

### create

Create a read replica for a specified cluster.

| Flag | Description |
| :--- | :--- |
| --cluster-name | Required. Name of the cluster to which you want to add read replicas. |
| --replica | Specifications for the read replica provided as key-value pairs.<br>Arguments:<br><ul><li>num-cores - number of vCPUs per node. Default is 2.</li><li>memory-mb - memory (MB) per node. Default is 4096.</li><li>disk-size-gb - disk size (GB) per node. Default is 10.</li><li>code - cloud provider (`AWS` or `GCP`)</li><li>region - region in which to deploy the read replica</li><li>num-nodes - number of nodes for the read replica. Default is 1.</li><li>vpc-name - name of the VPC in which to deploy the read replica</li><li>num-replicas - the replication factor</li><li>multi-zone - whether the read replica is multi-zone (`true` or `false`). Default is false.</li></ul>

### delete

Delete read replicas of a specified cluster.

| Flag | Description |
| :--- | :--- |
| --cluster-name | Required. Name of the cluster whose read replicas you want to delete. |

### list

List the read replicas of a specified cluster.

| Flag | Description |
| :--- | :--- |
| --cluster-name | Required. Name of the cluster whose read replicas you want to fetch. |

### update

Update a read replica for a specified cluster.

| Flag | Description |
| :--- | :--- |
| --cluster-name | Required. Name of the cluster with read replicas you want to update. |
| --replica | Specifications for the read replica provided as key-value pairs.<br>Arguments:<br><ul><li>num-cores - number of vCPUs per node. Default is 2.</li><li>memory-mb - memory (MB) per node. Default is 4096.</li><li>disk-size-gb - disk size (GB) per node. Default is 10.</li><li>code - cloud provider (`AWS` or `GCP`)</li><li>region - region in which to deploy the read replica</li><li>num-nodes - number of nodes for the read replica. Default is 1.</li><li>vpc-name - name of the VPC in which to deploy the read replica</li><li>num-replicas - the replication factor</li><li>multi-zone - whether the read replica is multi-zone (`true` or `false`). Default is false.</li></ul>
