# Build stage: compile the server (the Lab page's live benchmark runs are disabled in this image).
FROM gcc:13 AS build
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tests tests
RUN g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Iinclude src/server.cpp -o chronos_server -pthread -static-libstdc++ -static-libgcc \
 && g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Iinclude tests/test.cpp -o chronos_tests \
 && ./chronos_tests 12 800

FROM debian:bookworm-slim
RUN useradd -m app
WORKDIR /app
COPY --from=build /src/chronos_server .
COPY web2 web2
COPY results/*.json results/
USER app
EXPOSE 8080
# PORT is set by the host (Render etc.); the simulator pauses itself 2 minutes after the last viewer leaves.
CMD ["./chronos_server", "--host", "0.0.0.0", "--web", "web2", "--bench", "none", "--results", "results", "--max-active", "200000", "--idle-stop", "120"]
