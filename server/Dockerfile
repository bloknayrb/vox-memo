FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv

WORKDIR /app

# Install dependencies (cached layer — only busts when lock file changes)
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev --no-install-project

# Copy application code
COPY server.py config.py ./

RUN adduser --system --no-create-home appuser
USER appuser

EXPOSE 8000

CMD ["uv", "run", "python", "server.py"]
