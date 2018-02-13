===================================================================
rados_grace_tool -- manipulate the shared grace management database
===================================================================

SYNOPSIS
===================================================================

| rados_grace_tool [ --pool pool_id ] [ --name obj_id ] dump|start|join|lift|remove|enforce|noenforce [ hostname ... ]

DESCRIPTION
===================================================================

This tool allows the administrator to directly manipulate the database
used by the rados_cluster recovery backend. Cluster nodes use that database to
indicate their current state in order to coordinate a cluster-wide grace
period.

The first argument should be a command to execute against the database.
Any remaining arguments represent the hostnames of nodes in the cluster
that should be acted upon.

OPTIONS
===================================================================
**--pool**

Set the RADOS poolid in which the grace database object resides

**--name**

Set the name of the grace database RADOS object

COMMANDS
===================================================================

**dump**

Dump the current status of the grace period database to stdout. This
will show the current and recovery epoch serial numbers, as well as a
list of hosts currently in the cluster and what flags they have set
in their individual records.

**start**

Start a new grace period. This will begin a new grace period in the
cluster if one is not already active and set the record for the listed
cluster hosts as both needing a grace period and enforcing the grace
period. If a grace period is already active, then this is equivalent
to **join**.

**join**

Attempt to join an existing grace period. This works like **start**, but
only if there is already an existing grace period in force.

**lift**

Attempt to lift the current grace period. This will clear the need grace
flags for the listed hosts. If there are no more hosts in the cluster
that require a grace period, then it will be fully lifted and the cluster
will transition to normal operations.

**remove**

Remove one or more existing hosts from the cluster. This will remove the
listed hosts from the grace database, possibly lifting the current grace
period if there are no more hosts that need one.

**enforce**

Set the flag for the given hosts that indicates that they are currently
enforcing the grace period; not allowing the acquisition of new state by
clients.

**noenforce**

Clear the enforcing flag for the given hosts, meaning that those hosts
are now allowing clients to acquire new state.

STARTING A NEW CLUSTER FROM A DOWN STATE
===================================================================
Because cluster nodes will attempt to lift the grace period as soon as
no one needs it, it's best to start the grace period before bringing up any
nodes with an initial set of hosts that will be present. This ensures that
the grace period won't be lifted before all of the hosts have joined the
cluster.

Assuming that the nodes in our cluster are called host1 through host3:

        **rados_grace_tool start host1 host2 host3**

That will begin a new cluster-wide grace period, and add/update records for
all three hosts to indicate that they need the grace period and are
currently enforcing. With those records in place, the grace period can't
be lifted until they have all ended their local recovery periods.

After this point, new nodes can then join the cluster as needed. Cluster
nodes can more or less do so on their own so no special steps should be
needed to bring new nodes into an already-running cluster.

REMOVING A NODE FROM THE CLUSTER
===================================================================
To remove a node from the cluster, take it down and then execute the remove
command with the hosts to be removed:

        **rados_grace_tool remove host2 host3**

This will remove the node's record from the database, and possibly lift the
current grace period if the listed hosts were the last ones to need it.
