from .base import Provider


class OpenAIProvider(Provider):
    auth_header = "Authorization"
    auth_scheme = "Bearer"
