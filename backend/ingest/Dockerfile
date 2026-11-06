FROM node:22-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --omit=dev --silent
COPY ingest.mjs .
CMD ["node", "ingest.mjs"]
