"""blobhttp — enterprise HTTP client with rate limiting, SPNEGO auth, and connection pooling."""

__version__ = "0.1.0"

from .blobhttp_ext import (
    HttpClient,
    negotiate_token,
    negotiate_available,
)
