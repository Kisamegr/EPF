"""
Server-side pieces of the e-paper photo frame.

  config    the defaults, the live settings, and the config.yaml watcher
  state     in-memory runtime state shared between requests
  eventlog  the rolling record of check-ins and changes
  tracking  which photos have already been shown
  immich    talking to Immich, and choosing which photo comes next
  imaging   crop, enhance, dither and pack for the panel
  battery   voltage to percentage
  ota       staging and managing ESP32 OTA firmware updates

app.py holds the Flask app and the HTTP routes.
"""

