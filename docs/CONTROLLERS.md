# Playlister: for controllers and scripts

Reference for controllers, scripts and plugins that read or drive [Playlister](../README.md). Back to the [README](../README.md#for-controllers-and-scripts).

What plays now and what comes next can be read without the Properties window, so a controller key can show it.
Only names are published, never paths.

## obs-websocket

Playlister registers the vendor **`obs-playlister`** with obs-websocket (which ships with OBS). Where obs-websocket
is absent or off, nothing is registered.

Request `get_now_next`, sent as obs-websocket's `CallVendorRequest` with `vendorName` `obs-playlister` and
`requestType` `get_now_next`:

| Request field | Type | Meaning |
| :--- | :--- | :--- |
| `source_uuid` | string, optional | The Playlister source to ask, by its UUID |
| `source` | string, optional | The same by its name; used when `source_uuid` is absent |

With one of the two, the response has `found` (bool: whether that is a Playlister source) and, when it is, the
fields below. With neither, the response has `sources`: an array with these fields for every Playlister source.

| Response field | Type | Meaning |
| :--- | :--- | :--- |
| `source` | string | The source's name |
| `source_uuid` | string | The source's UUID |
| `playing` | string | The name of the item that is loaded: a playlist entry's title, else the file name, else the last part of the address. Empty when nothing is loaded |
| `next` | string | The name of the item that follows; while nothing plays, the item Play starts. Empty when nothing follows or it is not decided |
| `next_decided` | bool | False when Shuffle is on and the next item is chosen only when it starts |
| `state` | string | `playing`, `paused`, `stopped`, `ended`, `opening`, `error` or `none` |
| `status` | string | The worst of the Status lines: `ok`, `warning` or `error` |
| `row` | number | The playing item's row in the Playlist, from 1; 0 when nothing is loaded |
| `item` | number | Its place within a folder or playlist row, from 1; 0 for a file row |
| `items` | number | How many items the whole list has to play |

Event `now_next_changed` (obs-websocket's `VendorEvent`, `vendorName` `obs-playlister`, `eventType`
`now_next_changed`): sent when `playing`, `next`, `next_decided` or `status` of a source changes. Its `eventData`
has the same fields as the response for one source. Play, pause and stop alone send no event; obs-websocket's own
media events and `GetMediaInputStatus` report those.

## In OBS (scripts and plugins)

The same reading is a proc on the source:

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
calldata_t cd = {0};
proc_handler_call(ph, "get_now_next", &cd);
const char *playing = calldata_string(&cd, "playing"); /* also: next, state, status */
bool next_decided = calldata_bool(&cd, "next_decided");
long long row = calldata_int(&cd, "row"); /* also: item, items */
calldata_free(&cd);
```

and a signal on the source's signal handler when it changes:
`void playlister_now_next(ptr source, string playing, string next, bool next_decided, string status)`.

To select an item from a script or another plugin:

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
struct calldata cd = {0};
calldata_set_int(&cd, "media_index", 3);       // 4th row of the playlist
calldata_set_int(&cd, "folder_item_index", 3); // 4th item of that row if it is a folder
proc_handler_call(ph, "select_index", &cd);
calldata_free(&cd);
```

- `media_index`: 0-based row in the playlist. If it is past the end of the list, the call does nothing (the
  original README said it falls back to 0; the code never did).
- `folder_item_index`: 0-based item in the folder at `media_index`. If it is past the end of the folder, item 0 is
  played. It is ignored for rows that are not folders.

The source id is `obs_playlister_source`. To see the setting keys, use
[obs_data_get_json](https://docs.obsproject.com/reference-settings#c.obs_data_get_json) on the source's settings
or look at the scene collection JSON. The order of a folder is saved as `sort_key` (0 date, 1 name, 2 as listed)
and `sort_descending`; the Order list of Properties follows them.
