**TRANSPARENT RECOVERY PROTOTYPE** [Initial Stage, NOT TO BE MERGED]

**NOTE:** This module is incomplete, but working model for the idea described below.

**GOAL:** The idea is to get rid of grace period & its repercussions viz. non-reclaim type of requests not being entertained immediatedely after the server starts/restarts. There is a wait period of GRACE PERIOD (90 seconds).

**IDEA:** Design a mechanism that can help server to figure out whether a request recieved during grace period conflicts with a state that was held by a client in previous life of the server. Having done that server can entertain reclaim as well as non-reclaim non-conflicting requests during the grace period.

**Requirements:**
	
* A stable storage is required for storing the state information, so that it persists across server failures.
* Interface to access the stable storage with less computational overhead.
* A way to represent different type of state(s) with minimum possible memory overhead.
* A way to check the conflicts between persisting states & new requests during grace period.

Having all the above requirements met, when there is a new state creation, just persist the state in the stable storage.
At the time of restart, fetch the state(s) from the stable storage and check for the conflicts before denying/entertaining the requests.

**Implementation:**

* **docDB** (docDB.h and docBD.c): created a simple interface on the top of redis instance that can be used to store simple (key-value) pairs as well as (primary key - a json object) pair. For both kind it provides tradition CRUD (Create, Read, Update, Delete) operations.

* **cid_cowner_mapper** (cid_cowner_mapper.c): implements the methods to map clientid to client-owner, which is done at the time of exchange operation & the mapped values (cowner) are used at the time of storing the state. This is required because at every start the clientid changes, so in order to have relation between two clientid(s) in two different starts of server.

* **open_recovery** (open_recovery.c): implements three simple interfaces for storing open state in DB, fetching open state from DB & checking conflicting opens.

* **lock_recovery** (lock_recovery.c): implements three simple interfaces for storing byte-range locking state in DB, fetching locking state from DB & checking conflicting locks.

* **transparent_recovery** (transparent_recovery.h): this module has the declarations for all the interfaces required for transparent recovery.