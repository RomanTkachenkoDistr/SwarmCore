# Historical records

The history module is responsible for interfacing with "history archives",
loosely defined as "places that flat files can be stored and retrieved from".

History archives are defined by the user by adding sections of the form
`[HISTORY.name]` to the config file, containing `get` and `put` commands. The
`name` of each history archive is unimportant; it is simply used as a symbolic
logging prefix, and to permit more than one history archive in a single config
file.

The `get` and `put` commands define subprocesses that `stellar-core` will
execute in order to deposit historical records and retrieve them when catching
up. The form of each `get` or `put` command specified in the config file is a
string template defining a command to execute, with template parameters `{0}`
and `{1}` in place of the files being retrieved or transmitted.

For example, the following entry in a config file would define a history archive
called `s3stellar` based on the [AWS S3](https://aws.amazon.com/s3/) storage system:

~~~~
[HISTORY.s3stellar]
get="curl http://history.stellar.org/{0} -o {1}"
put="aws s3 cp {0} s3://history.stellar.org/{1}"
~~~~

In this example, `stellar-core` will use the `curl` command to fetch history and
the `aws` utility to publish history.

Records are generally XDR files ([RFC 4506](https://tools.ietf.org/html/rfc4506))
compressed with gzip ([RFC 1952](https://tools.ietf.org/html/rfc1952)).
The XDR definitions are stored in the [src/xdr](../xdr) directory.

History is generated by the operation of the [LedgerManager](../ledger), and
multiple peers may publish history to multiple archives. If the LedgerManager
detects that it is out of sync with its peers, it enters "catchup mode", during
which the history module downloads and replays historical history _from_ a
history archive _to_ the LedgerManager, attempting to return it to
synchronization with its peers.