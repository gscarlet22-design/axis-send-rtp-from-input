ARG ARCH=armv7hf
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

COPY ./app /opt/app/
WORKDIR /opt/app

# Windows checkouts and WSL mounts can leave CRLF or 777 permissions.
# 777 files make AXIS OS skip reverse-proxy rules.
RUN find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.json' -o -name 'Makefile' -o -name '*.html' -o -name 'LICENSE' \) -exec sed -i 's/\r$//' {} + \
    && find . -type f -exec chmod 644 {} + \
    && find . -type d -exec chmod 755 {} +

RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./
