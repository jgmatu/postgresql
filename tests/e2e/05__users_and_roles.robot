*** Settings ***
Documentation     User management by the admin, and role enforcement for a normal user (HTTP and PostgreSQL).
Resource          resources/api.resource


*** Variables ***
${NORMAL_PASSWORD}    normal-user-password


*** Test Cases ***
Admin creates a normal user
    &{extra}=    Create Dictionary    PGDEMO_NEW_PASSWORD=${NORMAL_PASSWORD}
    ${result}=    Run Client With    ${extra}    users    add    alice    normal
    Should Be Equal As Integers    ${result.rc}    0

Duplicate user is 409
    New User Should Be Rejected    409    alice    another-password

Invalid username is 422
    New User Should Be Rejected    422    Bad Name!    another-password

Short password is 400
    New User Should Be Rejected    400    bob    short

Last admin cannot be deleted
    ${headers}=    Auth Headers
    HTTP Status Should Be    422    DELETE    /api/users/admin    headers=${headers}

Normal user login
    Login As    alice    ${NORMAL_PASSWORD}

Normal user reads data
    ${headers}=    Auth Headers
    HTTP Status Should Be    200    GET    /api/data/dummy1    headers=${headers}

Normal user reads config status
    ${headers}=    Auth Headers
    HTTP Status Should Be    200    GET    /api/config    headers=${headers}

Normal user cannot list users (403)
    ${headers}=    Auth Headers
    HTTP Status Should Be    403    GET    /api/users    headers=${headers}

Normal user cannot set config (403)
    ${headers}=    Auth Headers    content_type=application/json
    HTTP Status Should Be    403    POST    /api/config    headers=${headers}    data={"target":"dummy1"}

Me reports demo_normal
    ${me}=    Client Json    me
    Should Be Equal    ${me}[pg_role]    demo_normal


*** Keywords ***
New User Should Be Rejected
    [Arguments]    ${expected}    ${username}    ${password}
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    username=${username}    password=${password}    role=normal
    HTTP Status Should Be    ${expected}    POST    /api/users    headers=${headers}    json=${body}
