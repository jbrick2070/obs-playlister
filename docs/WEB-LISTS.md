# Playlister: web lists, passwords and proxies

How [Playlister](../README.md) reads a playlist from a web address. Back to [Options](../README.md#options).

- **User name and password:** a list address may carry them (`https://user:password@host/list.m3u`). They are sent
  to that server, and after a redirect only if it stays with the same scheme, host and port: never to another host,
  nor from http to https. A redirect from https to http is refused.
- **Where they show:** Playlister leaves them out of the **Play item** labels, **Status**, **Now playing**,
  **Next**, the OBS log and the names it sends to controllers. The **Playlist** itself shows each address as you
  typed it, and the scene collection file keeps it that way, since that is what Playlister plays and finds the
  item by.
- **Special characters:** write a `/`, `?` or `#` in a user name or password as `%2F`, `%3F` or `%23`, as in any
  web address. Otherwise the address ends at that character, so it may go to another server than you meant, and
  that part of the user name or password may show in those labels.
- **Character set:** a list that is not UTF-8 is read in the character set its server names. It is read as
  Windows-1252 when the server names none, or names one this computer cannot convert from, or the text is not
  valid in the one it names.
- **Proxy:** Playlister uses the proxy the environment names (`http_proxy` for http addresses, `https_proxy` for
  https, `all_proxy` for either that has none of its own, `no_proxy` for hosts to reach directly), for each
  address of a redirect chain by itself. With none named, Windows uses the system's proxy settings (but reaches a
  host named in `no_proxy` directly); macOS and Linux use none.
- **`no_proxy` on Windows** takes host names and addresses (IPv6 bare or in brackets), with or without a port, and
  `*` for every host. A port is compared with the list address's own, not with one a redirect leads to. Address
  ranges such as `10.0.0.0/8` are not supported there.
- **The items themselves** are played by OBS's own media source (FFmpeg), which looks at `http_proxy` and
  `no_proxy` only, so set those for the items to play as well.
