# makensis for the Windows installer (scripts/release/makensis), so the build machine needs no NSIS.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends nsis && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["makensis"]
