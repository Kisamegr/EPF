FROM python:3.9-slim-bookworm AS builder
WORKDIR /build
COPY requirements.txt setup.py cpy.pyx /build/
RUN apt-get update && apt-get install -y --no-install-recommends build-essential \
    && pip install --no-cache-dir -r requirements.txt cython setuptools \
    && python setup.py build_ext --inplace \
    && rm -rf /var/lib/apt/lists/*

FROM python:3.9-slim-bookworm

# Set working directory
WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Explicit runtime-only copies keep workstation secrets and firmware outputs out.
COPY --from=builder /build/cpy*.so /app/
COPY app.py /app/
COPY epf /app/epf
COPY templates /app/templates
COPY static /app/static

RUN useradd --system --create-home --uid 10001 epf && chown -R epf:epf /app
USER epf

# Exposed Flask port
EXPOSE 5000

# Environment variables
# IMMICH API KEY
ENV PYTHONUNBUFFERED=1

# Default command
CMD ["gunicorn", "--bind", "0.0.0.0:5000", "--workers", "1", "--threads", "4", "--timeout", "60", "app:app"]
