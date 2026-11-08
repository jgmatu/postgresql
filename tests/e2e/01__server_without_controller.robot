*** Settings ***
Documentation     Server alone: authentication, and a configuration request queued while the controller is down.
Resource          resources/api.resource


*** Test Cases ***
Health is public
    HTTP Status Should Be    200    GET    /api/health

Data without token is 401
    HTTP Status Should Be    401    GET    /api/data/dummy1

Garbage token is 401
    &{headers}=    Create Dictionary    Authorization=Bearer x.y.z
    HTTP Status Should Be    401    GET    /api/data/dummy1    headers=${headers}

Wrong password is 401
    &{body}=    Create Dictionary    username=admin    password=wrong-password
    HTTP Status Should Be    401    POST    /api/auth/login    json=${body}

Unknown user is 401
    &{body}=    Create Dictionary    username=nobody    password=wrong-password
    HTTP Status Should Be    401    POST    /api/auth/login    json=${body}

Admin login
    Login As    admin    ${ADMIN_PASSWORD}

Config set without controller is queued (202 PENDING)
    ${queued}=    Client Json    config    set    dummy1    rate_ms=300
    Should Be Equal    ${queued}[status]    PENDING
    Set Global Variable    ${QUEUED_ID}    ${queued}[request_id]
