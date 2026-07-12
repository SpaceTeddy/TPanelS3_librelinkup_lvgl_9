# Extra root CAs

Certificates here are merged into `data/cert/x509_crt_bundle.bin` in addition to
the current Mozilla CA set (see `tools/update_crt_bundle.sh`).

The ESP-IDF certificate-bundle verifier (`esp_crt_bundle_attach`) matches a
server's terminal certificate purely by *issuer name* against the bundle. If a
server sends a cross-signed root instead of a self-signed one, the bundle must
contain the cross-signing issuer too, even if Mozilla no longer ships it,
otherwise validation fails even though the "real" root is present in the
bundle under its self-signed form.

- `globalsign_root_ca_r1.pem` — GlobalSign Root CA (R1), valid until
  2028-01-28. Mozilla dropped this from its CA bundle, but `api.libreview.io`
  (LibreLinkUp backend) still serves a GTS Root R4 certificate cross-signed by
  this root instead of the self-signed one. Verified with:
  `openssl verify -CAfile globalsign_root_ca_r1.pem <api.libreview.io leaf+intermediate>`
  Remove once LibreView serves the self-signed GTS Root R4 (or its cross-sign
  issuer changes) — check with:
  `echo | openssl s_client -connect api.libreview.io:443 -servername api.libreview.io -showcerts`
