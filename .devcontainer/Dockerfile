# Ambiente de firmware do projeto IKNET – baseado no template oficial da
# extensão ESP-IDF (espressif/vscode-esp-idf-extension, templates/.devcontainer).
# Imagem oficial da Espressif com ESP-IDF, compiladores e Python já instalados.
ARG DOCKER_TAG=v5.5.6
FROM espressif/idf:${DOCKER_TAG}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update -y && apt-get install udev -y

RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> ~/.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["/bin/bash", "-c"]
