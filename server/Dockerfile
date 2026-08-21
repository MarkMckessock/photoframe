# Build context is the REPO ROOT, not this directory -- the image needs the shared
# `pfrm` package as well as the service, and keeping the in-container layout identical
# to the repo layout means app.py's imports work the same in both places.
#
#   docker build -f server/Dockerfile -t photoframe .
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /app

COPY server/requirements.txt ./server/requirements.txt
RUN pip install --no-cache-dir -r server/requirements.txt

COPY pfrm/ ./pfrm/
COPY server/__init__.py server/app.py ./server/

EXPOSE 5000

# One worker on purpose. The ETag cache and the "which blob is current" question are
# far simpler with a single process, and the workload is a handful of requests an hour
# -- threads are plenty for the concurrency that actually exists. The 60 s timeout
# gives dithering a 1.9 megapixel image room even on a busy node.
CMD ["gunicorn", "--bind", "0.0.0.0:5000", "--workers", "1", "--threads", "4", \
     "--timeout", "60", "--access-logfile", "-", "server.app:app"]
