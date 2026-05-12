# Build:
# docker build -o . .

# Use an official image
FROM devkitpro/devkitppc:20250527 AS usbloadergx
 
# Copy current folder into container, then compile
COPY . /projectroot/
RUN cd /projectroot && make zip -j$(nproc)

# Copy the ZIP file AND a loose boot.dol out of the container.
# Exporting boot.dol directly prevents a stale local `usbloader_gx/`
# folder from a previous manual extract being mistaken for the fresh
# build output.
FROM scratch AS export-stage
COPY --from=usbloadergx /projectroot/usbloader_gx.zip /
COPY --from=usbloadergx /projectroot/usbloader_gx/boot.dol /
