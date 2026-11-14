*** Settings ***
Documentation     The control panel on the server's own origin: every page is public static HTML,
...               while each ``/api`` path still needs a token and the right role. The admin pages
...               load for anyone; what protects them is the API answering 403 to a normal token.
...               Hashed bundles are cached for good, everything else is revalidated, and every
...               panel response carries the security headers. Unknown paths are 404: HTML for
...               the panel, JSON under ``/api``.
Resource          resources/api.resource
Suite Setup       Open The Panel Files
Suite Teardown    Destroy Stack


*** Variables ***
${CSP}            default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'


*** Test Cases ***
Panel pages are public HTML
    FOR    ${path}    IN    /    /devices/    /device/?name=dummy1    /config/    /map/    /pki/
    ...    /events/    /system/    /admin/devices/    /admin/identity/    /admin/config/    /admin/users/
        ${response}=    HTTP Status Should Be    200    GET    ${path}
        Should Start With    ${response.headers}[Content-Type]    text/html
        Should Contain    ${response.text}    <html lang="es"
    END

A route without its trailing slash is the same page
    ${with}=    HTTP Status Should Be    200    GET    /devices/
    ${without}=    HTTP Status Should Be    200    GET    /devices
    Should Be Equal    ${without.text}    ${with.text}

Pages carry the security headers and are revalidated
    ${response}=    HTTP Status Should Be    200    GET    /devices/
    Security Headers Should Be Set    ${response}
    Should Be Equal    ${response.headers}[Cache-Control]    no-cache

Hashed bundles are JavaScript and immutable
    ${response}=    HTTP Status Should Be    200    GET    ${BUNDLE}
    Should Be Equal    ${response.headers}[Content-Type]    text/javascript; charset=utf-8
    Should Be Equal    ${response.headers}[Cache-Control]    public, max-age=31536000, immutable
    Security Headers Should Be Set    ${response}

Route payloads are plain text
    ${response}=    HTTP Status Should Be    200    GET    /devices/__next._tree.txt
    Should Be Equal    ${response.headers}[Content-Type]    text/plain; charset=utf-8

VERSION names the commit the panel was built from
    ${response}=    HTTP Status Should Be    200    GET    /VERSION
    Should Match Regexp    ${response.text}    (?m)^commit=\\S+$
    Should Match Regexp    ${response.text}    (?m)^built_at=\\S+$

Unknown page is the 404 page with status 404
    ${response}=    HTTP Status Should Be    404    GET    /no-such-page/
    Should Start With    ${response.headers}[Content-Type]    text/html
    Should Contain    ${response.text}    data-testid="not-found"
    Security Headers Should Be Set    ${response}

Paths cannot leave the webroot
    FOR    ${path}    IN    /%2e%2e/%2e%2e/etc/passwd    /devices/%2e%2e/%2e%2e/etc/passwd    /..%2f..%2fetc/passwd
        HTTP Status Should Be    404    GET    ${path}
    END

Only GET reaches the panel
    ${response}=    HTTP Status Should Be    404    POST    /devices/
    Should Be Equal    ${response.json()}[error]    http_404

API without a token is 401
    FOR    ${path}    IN    /api/devices    /api/nope    /API/devices    /%61pi/devices    //api/devices
        ${response}=    HTTP Status Should Be    401    GET    ${path}
        Should Be Equal    ${response.headers}[Content-Type]    application/json
    END

Unknown API path is a JSON 404
    ${headers}=    Auth Headers
    ${response}=    HTTP Status Should Be    404    GET    /api/nope    headers=${headers}
    Should Be Equal    ${response.headers}[Content-Type]    application/json
    Should Be Equal    ${response.json()}[error]    not_found

Health and login stay public
    HTTP Status Should Be    200    GET    /api/health
    ${body}=    Create Dictionary    username=alice    password=wrong-password
    HTTP Status Should Be    401    POST    /api/auth/login    json=${body}

A normal token loads the admin pages but every admin write is 403
    ${headers}=    Normal User Headers
    HTTP Status Should Be    200    GET    /admin/devices/
    &{device}=    Create Dictionary    name=panel-x    type=sine
    HTTP Status Should Be    403    POST    /api/devices    headers=${headers}    json=${device}
    HTTP Status Should Be    403    DELETE    /api/devices/dummy1    headers=${headers}
    &{config}=    Create Dictionary    device=dummy1    rate_ms=${500}
    HTTP Status Should Be    403    POST    /api/config    headers=${headers}    json=${config}
    &{revoke}=    Create Dictionary    reason=certificateHold
    HTTP Status Should Be    403    POST    /api/devices/dummy1/revoke    headers=${headers}    json=${revoke}
    HTTP Status Should Be    403    POST    /api/devices/dummy1/rotate    headers=${headers}
    HTTP Status Should Be    403    GET    /api/users    headers=${headers}
    &{user}=    Create Dictionary    username=panel-x    password=panel-x-password    role=normal
    HTTP Status Should Be    403    POST    /api/users    headers=${headers}    json=${user}
    HTTP Status Should Be    403    DELETE    /api/users/admin    headers=${headers}
    HTTP Status Should Be    404    GET    /api/devices/panel-x    headers=${headers}


*** Keywords ***
Open The Panel Files
    Arm Fleet
    Ensure Alice
    Find A Bundle

Find A Bundle
    [Documentation]    A hashed script that ``/devices/`` loads; its name changes with every build.
    ${response}=    HTTP Status Should Be    200    GET    /devices/
    ${matches}=    Get Regexp Matches    ${response.text}    /_next/static/[^"]+\\.js
    Should Not Be Empty    ${matches}    /devices/ loads no /_next/static script
    Set Suite Variable    ${BUNDLE}    ${matches}[0]

Security Headers Should Be Set
    [Arguments]    ${response}
    Should Be Equal    ${response.headers}[X-Content-Type-Options]    nosniff
    Should Be Equal    ${response.headers}[X-Frame-Options]    DENY
    Should Be Equal    ${response.headers}[Referrer-Policy]    no-referrer
    Should Be Equal    ${response.headers}[Content-Security-Policy]    ${CSP}
