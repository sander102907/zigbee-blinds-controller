const exposes = require('zigbee-herdsman-converters/lib/exposes');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

const e = exposes.presets;
const ea = exposes.access;
const manufacturerCode = 0x131b;
const ATTR_TRAVEL_TIME_MS = 0xf010;
const ENDPOINT_ID = 10;

function parseTravelTime(rawValue) {
  if (rawValue === undefined || rawValue === null) {
    return undefined;
  }

  const value = Number(rawValue);
  if (!Number.isFinite(value)) {
    return undefined;
  }

  return Math.round(value);
}

function getTravelTime(data) {
  const directValue =
    parseTravelTime(data[ATTR_TRAVEL_TIME_MS]) ??
    parseTravelTime(data[`0x${ATTR_TRAVEL_TIME_MS.toString(16)}`]) ??
    parseTravelTime(data.travel_time_ms);

  if (directValue !== undefined) {
    return directValue;
  }

  const matchingEntry = Object.entries(data).find(([key]) => Number(key) === ATTR_TRAVEL_TIME_MS);
  return matchingEntry ? parseTravelTime(matchingEntry[1]) : undefined;
}

module.exports = {
  zigbeeModel: ['Blind_Tilt_Controller'],
  model: 'Blind_Tilt_Controller',
  vendor: 'Espressif',
  description: 'Battery-powered blind / tilt actuated cover with configurable travel time',
  meta: {
    coverStateFromTilt: true,
  },
  exposes: [
    e.cover_position_tilt().removeFeature('position'),
    e.battery(),
    e.numeric('travel_time_ms', ea.ALL)
      .withUnit('ms')
      .withValueMin(500)
      .withValueMax(60000)
      .withDescription('Full travel time for a 0-100% move'),
  ],
  fromZigbee: [
    fz.cover_position_tilt,
    fz.battery,
    function (model, msg, publish, options, meta) {
      if (!msg || !msg.data) {
        return;
      }

      const value = getTravelTime(msg.data);

      const tilt = parseTravelTime(msg.data.currentPositionTiltPercentage);
      const result = {};

      if (tilt !== undefined && tilt <= 100) {
        result.tilt = tilt;

        if (tilt === 0) {
          result.state = 'OPEN';
        } else if (tilt === 100) {
          result.state = 'CLOSE';
        }
      }

      if (value !== undefined) {
        result.travel_time_ms = value;
      }

      return Object.keys(result).length > 0 ? result : undefined;
    },
  ],
  toZigbee: [
    tz.cover_state,
    tz.cover_position_tilt,
    {
      key: ['travel_time_ms'],
      convertSet: async (entity, key, value) => {
        const numericValue = Number(value);
        if (!Number.isFinite(numericValue) || numericValue < 500 || numericValue > 60000) {
          throw new Error('travel_time_ms must be between 500 and 60000 milliseconds');
        }

        const travelTimeMs = Math.round(numericValue);
        await entity.write('closuresWindowCovering', {
          [ATTR_TRAVEL_TIME_MS]: {
            value: travelTimeMs,
            type: 0x23,
          },
        }, {
          manufacturerCode,
        });

        return {
          state: {
            travel_time_ms: travelTimeMs,
          },
        };
      },
      convertGet: async (entity) => {
        await entity.read('closuresWindowCovering', [ATTR_TRAVEL_TIME_MS], {
          manufacturerCode,
        });
      },
    },
  ],
  configure: async (device, coordinatorEndpoint, logger) => {
    const endpoint = device.getEndpoint(ENDPOINT_ID);
    if (!endpoint) {
      throw new Error(`Endpoint ${ENDPOINT_ID} was not found`);
    }

    const configure = async (operation, description) => {
      try {
        await operation();
      } catch (error) {
        if (logger && logger.warn) {
          logger.warn(`Unable to configure ${description}: ${error.message}`);
        }
      }
    };

    await configure(
      () => reporting.bind(endpoint, coordinatorEndpoint, ['closuresWindowCovering', 'genPowerCfg']),
      'Window Covering binding',
    );
    await configure(() => endpoint.configureReporting('genPowerCfg', [{
      attribute: 'batteryPercentageRemaining',
      minimumReportInterval: 60,
      maximumReportInterval: 3600,
      reportableChange: 1,
    }]), 'battery reporting');
    await configure(() => endpoint.configureReporting('closuresWindowCovering', [{
      attribute: 'currentPositionTiltPercentage',
      minimumReportInterval: 0,
      maximumReportInterval: 300,
      reportableChange: 1,
    }]), 'tilt reporting');

    const reads = [
      ['genPowerCfg', ['batteryPercentageRemaining']],
      ['closuresWindowCovering', ['currentPositionTiltPercentage']],
      ['closuresWindowCovering', [ATTR_TRAVEL_TIME_MS], {manufacturerCode}],
    ];

    for (const [cluster, attributes, options] of reads) {
      try {
        await endpoint.read(cluster, attributes, options);
      } catch (error) {
        if (logger && logger.debug) {
          logger.debug(`Unable to read ${cluster} attributes: ${error.message}`);
        }
      }
    }
  },
};
