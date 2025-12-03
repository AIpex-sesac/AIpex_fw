from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional

DESCRIPTOR: _descriptor.FileDescriptor

class WakeUpRequest(_message.Message):
    __slots__ = ("script_name", "args")
    SCRIPT_NAME_FIELD_NUMBER: _ClassVar[int]
    ARGS_FIELD_NUMBER: _ClassVar[int]
    script_name: str
    args: str
    def __init__(self, script_name: _Optional[str] = ..., args: _Optional[str] = ...) -> None: ...

class WakeUpResponse(_message.Message):
    __slots__ = ("success", "message", "process_id")
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    PROCESS_ID_FIELD_NUMBER: _ClassVar[int]
    success: bool
    message: str
    process_id: int
    def __init__(self, success: bool = ..., message: _Optional[str] = ..., process_id: _Optional[int] = ...) -> None: ...
