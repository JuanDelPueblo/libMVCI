# Filter and clear contracts

## Supported filters

`PassThruStartMsgFilter` supports `PASS_FILTER` with one mask byte and one
pattern byte. It refuses `BLOCK_FILTER`, `FLOW_CONTROL_FILTER`, unknown types,
empty or unequal lengths, and longer descriptors. It also refuses a flow
control message for PASS and any nonzero filter `TxFlags`. Refusal sends no
command and leaves the output ID unchanged.

Both descriptor protocol IDs must agree. A nonzero ID must match the channel.
The existing pair of zero-initialized protocol IDs remains accepted. The
channel supplies the wire protocol in this form.

The driver assigns public filter IDs separately from its device handles.
Stopping a filter frees its device-handle slot. A new filter in that slot gets
a new public ID, so an old ID cannot stop the replacement during that session.
The bookkeeping limit is 32 active filters. This is a software limit, not a
measurement of cable capacity. A full table returns `ERR_EXCEEDED_LIMIT`
without sending a command.

`PassThruStopMsgFilter` sends the existing stop command with the device handle
used at start. It removes the entry only when the session helper reports
success. A failed stop retains the entry for retry.

## Clear operations

All channel IOCTLs require a connected channel. `READ_VBATT` and
`READ_PROG_VOLTAGE` require a device ID and remain unsupported. A device ID
cannot substitute for a channel ID, or a channel ID for a device ID.

- `CLEAR_MSG_FILTERS` stops each active filter with the existing stop command.
  It removes each successfully stopped entry. On failure it returns
  `ERR_FAILED` and retains the failed and unvisited entries. A retry addresses
  those same device handles. An empty table needs no command.
- `CLEAR_PERIODIC_MSGS` uses the existing clear-periodic command and reports
  the session helper's result. It does not add periodic transmission support.
- `CLEAR_RX_BUFFER` clears the software receive queue and requests the host
  serial input purge through the existing session helper. It sends no cable
  command. It does not guarantee that a cable mailbox is empty or that later
  polling cannot deliver a message already held by the cable. The current
  transport purge API does not report OS purge failures.
- `CLEAR_TX_BUFFER` returns `ERR_NOT_SUPPORTED`. There is no driver TX queue
  or evidenced cable cancellation command. An ACK for a prior write does not
  prove that the cable has finished transmission. No cancellation, mailbox
  flush or physical TX timing guarantee is made.

`GET_CONFIG` remains limited to the host `LOOPBACK` delivery policy. A
successful setting ACK does not enable cable timing read-back.

Offline scripted tests cover these API rules and transport failure handling.
They do not prove physical timing, cable filter capacity or ECU behavior.
Filter and periodic bookkeeping relies on session reply validation. That layer
must reject malformed replies and failure status bytes before it reports
success; the public filter implementation does not add a second wire parser.
