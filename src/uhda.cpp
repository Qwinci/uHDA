#include "uhda/uhda.h"
#include "uhda/kernel_api.h"
#include "controller.hpp"
#include "utils.hpp"
#include "spec.hpp"

namespace {
	struct Device {
		uint16_t vendor;
		uint16_t device;
	};

	constexpr Device MATCH_TABLE[] {
		UHDA_MATCHING_DEVICES
	};
}

using namespace uhda;

bool uhda_device_matches(uint16_t vendor, uint16_t device) {
	for (auto match : MATCH_TABLE) {
		if (match.vendor == vendor && match.device == device) {
			return true;
		}
	}
	return false;
}

bool uhda_class_matches(uint16_t clazz, uint16_t subclass) {
	return clazz == UHDA_MATCHING_CLASS && subclass == UHDA_MATCHING_SUBCLASS;
}

UhdaStatus uhda_init(void* pci_device, UhdaController** res) {
	auto ptr = uhda_kernel_malloc(sizeof(UhdaController));
	if (!ptr) {
		return UHDA_STATUS_NO_MEMORY;
	}

	auto* controller = uhda::construct<UhdaController>(ptr, pci_device);
	auto status = controller->init();

	if (status != UHDA_STATUS_SUCCESS) {
		controller->~UhdaController();
		uhda_kernel_free(ptr, sizeof(UhdaController));
		return status;
	}

	*res = controller;

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_destroy(UhdaController* controller) {
	auto status = controller->destroy();
	controller->~UhdaController();
	uhda_kernel_free(controller, sizeof(UhdaController));
	return status;
}

UhdaStatus uhda_suspend(UhdaController* controller) {
	return controller->suspend();
}

UhdaStatus uhda_resume(UhdaController* controller) {
	return controller->resume();
}

void uhda_get_codecs(UhdaController* controller, const UhdaCodec* const** codecs, size_t* codec_count) {
	*codecs = controller->codecs.data();
	*codec_count = controller->codecs.size();
}

void uhda_get_output_streams(UhdaController* controller, UhdaStream*** streams, size_t* stream_count) {
	*streams = controller->out_stream_ptrs;
	*stream_count = controller->out_stream_count;
}

void uhda_codec_get_output_groups(
	const UhdaCodec* codec,
	const UhdaOutputGroup* const** output_groups,
	size_t* output_group_count) {
	*output_groups = codec->output_groups.data();
	*output_group_count = codec->output_groups.size();
}

void uhda_output_group_get_outputs(
	const UhdaOutputGroup* output_group,
	const UhdaOutput* const** outputs,
	size_t* output_count) {
	*outputs = output_group->outputs.data();
	*output_count = output_group->outputs.size();
}

UhdaOutputInfo uhda_output_get_info(const UhdaOutput* output) {
	UhdaOutputInfo info {};
	switch (output->widget->default_dev) {
		case default_dev::LINE_OUT:
			info.type = UHDA_OUTPUT_TYPE_LINE_OUT;
			break;
		case default_dev::SPEAKER:
			info.type = UHDA_OUTPUT_TYPE_SPEAKER;
			break;
		case default_dev::HP_OUT:
			info.type = UHDA_OUTPUT_TYPE_HEADPHONE;
			break;
		case default_dev::CD:
			info.type = UHDA_OUTPUT_TYPE_CD;
			break;
		case default_dev::SPDIF_OUT:
			info.type = UHDA_OUTPUT_TYPE_SPDIF_OUT;
			break;
		case default_dev::DIGITAL_OTHER_OUT:
			info.type = UHDA_OUTPUT_TYPE_OTHER_DIGITAL_OUT;
			break;
		default:
			info.type = UHDA_OUTPUT_TYPE_UNKNOWN;
			break;
	}

	return info;
}

bool uhda_paths_usable_simultaneously(const UhdaPath** paths, size_t count, bool same_stream) {
	for (size_t i = 0; i < count; ++i) {
		auto path = paths[i];

		for (size_t widget_i = 1; widget_i < path->widgets.size(); ++widget_i) {
			auto widget = path->widgets[widget_i];

			for (size_t j = 0; j < count; ++j) {
				if (j == i) {
					continue;
				}

				auto other_path = paths[j];
				for (size_t other_widget_i = 1; other_widget_i < other_path->widgets.size(); ++other_widget_i) {
					auto other_widget = other_path->widgets[other_widget_i];

					if (path->widgets[widget_i - 1] == other_path->widgets[other_widget_i - 1]) {
						if (!same_stream) {
							return false;
						}
					}
					else if (widget == other_widget) {
						return false;
					}
				}
			}
		}
	}

	return true;
}

UhdaStatus uhda_find_path(
	const UhdaOutput* dest,
	const UhdaPath** other_paths,
	size_t other_path_count,
	bool same_stream,
	UhdaPath** res) {
	auto& all_paths = dest->widget->codec->output_paths;
	for (auto& path : all_paths) {
		if (path.widgets.front() == dest->widget) {
			bool not_usable = false;

			for (size_t i = 0; i < other_path_count; ++i) {
				const UhdaPath* paths[] {&path, other_paths[i]};
				auto usable = uhda_paths_usable_simultaneously(paths, 2, same_stream);
				if (!usable) {
					not_usable = true;
					break;
				}
			}

			if (not_usable) {
				continue;
			}

			*res = &path;
			return UHDA_STATUS_SUCCESS;
		}
	}

	return UHDA_STATUS_UNSUPPORTED;
}

static PcmFormat pcm_format_from_params(UhdaStreamParams* params) {
	PcmFormat fmt {};
	params->sample_rate = fmt.set_sample_rate(params->sample_rate);
	params->channels = fmt.set_channels(params->channels);

	uint8_t bits;
	switch (params->fmt) {
		case UHDA_FORMAT_PCM8:
			bits = 8;
			break;
		case UHDA_FORMAT_PCM16:
			bits = 16;
			break;
		case UHDA_FORMAT_PCM20:
			bits = 20;
			break;
		case UHDA_FORMAT_PCM24:
			bits = 24;
			break;
		case UHDA_FORMAT_PCM32:
			bits = 32;
			break;
	}

	uint8_t real_bits = fmt.set_bits_per_sample(bits);
	if (real_bits != bits) {
		switch (real_bits) {
			case 8:
				params->fmt = UHDA_FORMAT_PCM8;
				break;
			case 16:
				params->fmt = UHDA_FORMAT_PCM16;
				break;
			case 20:
				params->fmt = UHDA_FORMAT_PCM20;
				break;
			case 24:
				params->fmt = UHDA_FORMAT_PCM24;
				break;
			case 32:
				params->fmt = UHDA_FORMAT_PCM32;
				break;
			default:
				break;
		}
	}

	return fmt;
}

UhdaStatus uhda_path_setup(UhdaPath* path, UhdaStreamParams* params, UhdaStream* stream) {
	if (!stream->output) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	auto fmt = pcm_format_from_params(params);

	auto output = path->widgets.back();
	if (output->type != widget_type::AUDIO_OUT) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	auto codec = path->codec;

	auto status = codec->set_converter_format(output->nid, fmt.value);
	if (status != UHDA_STATUS_SUCCESS) {
		return status;
	}

	status = codec->set_converter_channel_count(output->nid, fmt.value & pcm_format::CHAN);
	if (status != UHDA_STATUS_SUCCESS) {
		return status;
	}

	for (size_t i = 0; i < path->widgets.size(); ++i) {
		auto widget = path->widgets[i];

		if (i > 0 && widget->connections.size() > 1) {
			auto prev_widget = path->widgets[i - 1];

			size_t index = 0;
			for (size_t j = 0; j < widget->connections.size(); ++j) {
				auto connection = widget->connections[j];
				if (connection & 1 << 7) {
					auto start = widget->connections[j - 1];
					auto end = connection & 0x7F;
					if (prev_widget->nid >= start && prev_widget->nid <= end) {
						index += prev_widget->nid - start;
						break;
					}
					index += end - start;
				}
				else {
					if (prev_widget->nid == connection) {
						break;
					}
					++index;
				}
			}

			status = codec->set_selected_connection(widget->nid, index);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}
		}

		status = codec->set_power_state(widget->nid, 0);
		if (status != UHDA_STATUS_SUCCESS) {
			return status;
		}

		if (widget->type == widget_type::PIN_COMPLEX) {
			if (widget->pin_caps & 1 << 16) {
				status = codec->set_eapd_enable(widget->nid, 1 << 1);
				if (status != UHDA_STATUS_SUCCESS) {
					return status;
				}
			}

			uint8_t step = widget->out_amp_caps & 0x7F;

			// set output amp, set left amp, set right amp and gain
			uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | step;
			status = codec->set_amp_gain_mute(widget->nid, amp_data);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}

			// headphone amp, out enable
			uint32_t pin_control = 1 << 7 | 1 << 6;
			status = codec->set_pin_control(widget->nid, pin_control);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
		else if (widget->type == widget_type::AUDIO_MIXER) {
			uint8_t step = widget->out_amp_caps & 0x7F;

			// set output amp, set left amp, set right amp and gain
			uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | step;
			status = codec->set_amp_gain_mute(widget->nid, amp_data);
			if (status == UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
		else if (widget->type == widget_type::AUDIO_OUT) {
			status = codec->set_converter_control(widget->nid, stream->index + 1, 0);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}

			uint8_t step = widget->out_amp_caps & 0x7F;

			// set output amp, set left amp, set right amp and gain
			uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | (step / 2);

			path->gain = step / 2;

			status = codec->set_amp_gain_mute(widget->nid, amp_data);
			if (status == UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
	}

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_path_shutdown(UhdaPath* path) {
	auto codec = path->codec;

	for (auto widget : path->widgets) {
		if (widget->type == widget_type::PIN_COMPLEX) {
			// set output amp, set left amp, set right amp and mute
			uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | 1 << 7;
			auto status = codec->set_amp_gain_mute(widget->nid, amp_data);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}

			status = codec->set_pin_control(widget->nid, 0);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
		else if (widget->type == widget_type::AUDIO_MIXER) {
			// set output amp, set left amp, set right amp and mute
			uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | 1 << 7;
			auto status = codec->set_amp_gain_mute(widget->nid, amp_data);
			if (status == UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
		else if (widget->type == widget_type::AUDIO_OUT) {
			auto status = codec->set_converter_control(widget->nid, 0, 0);
			if (status != UHDA_STATUS_SUCCESS) {
				return status;
			}
		}
	}

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_path_set_volume(UhdaPath* path, int volume) {
	if (volume > 100) {
		volume = 100;
	}

	auto output = path->widgets.back();
	if (output->type != widget_type::AUDIO_OUT) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	uint8_t max_value = output->out_amp_caps & 0x7F;
	uint8_t one_percentage = max_value / 100;
	if (one_percentage == 0) {
		one_percentage = 1;
	}
	uint32_t value = one_percentage * volume;
	if (value > max_value || volume == 100) {
		value = max_value;
	}

	path->gain = value;

	// set output amp, set left amp, set right amp and gain
	uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | value;
	return path->codec->set_amp_gain_mute(output->nid, amp_data);
}

UhdaStatus uhda_path_mute(UhdaPath* path, bool mute) {
	auto output = path->widgets.back();
	if (output->type != widget_type::AUDIO_OUT) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	// set output amp, set left amp, set right amp, mute and gain
	uint16_t amp_data = 1 << 15 | 1 << 13 | 1 << 12 | (mute ? (1 << 7) : 0) | path->gain;
	return path->codec->set_amp_gain_mute(output->nid, amp_data);
}

UhdaStatus uhda_stream_setup(
	UhdaStream* stream,
	const UhdaStreamParams* params,
	uint32_t ring_buffer_size,
	UhdaBufferFillFn buffer_fill_fn,
	void* arg) {
	if (!stream->output) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	stream->buffer_fill_fn = buffer_fill_fn;
	stream->buffer_fill_fn_arg = arg;

	auto params_copy = *params;

	auto fmt = pcm_format_from_params(&params_copy);

	stream->space.store(regs::stream::FMT, fmt.value);
	return stream->setup(ring_buffer_size);
}

UhdaStatus uhda_stream_shutdown(UhdaStream* stream) {
	stream->destroy();
	return UHDA_STATUS_SUCCESS;
}

#define memcpy __builtin_memcpy

UhdaStatus uhda_stream_play(UhdaStream* stream, bool play) {
	auto ctl0 = stream->space.load(regs::stream::CTL0);
	if (play) {
		if (!(ctl0 & sdctl0::RUN)) {
			uint32_t initial_amount = 0x1000 * 4;

			for (uint32_t i = 0; i < initial_amount; i += 0x1000) {
				memcpy(
					stream->buffer_pages[i / 0x1000],
					launder(static_cast<char*>(stream->ring_buffer) + i),
					0x1000);
			}
			stream->ring_buffer_size -= initial_amount;
			stream->ring_buffer_read_pos += initial_amount;
			stream->current_pos = initial_amount;

			ctl0 |= sdctl0::RUN(true);
			stream->space.store(regs::stream::CTL0, ctl0);
		}
	}
	else {
		if (ctl0 & sdctl0::RUN) {
			ctl0 &= ~sdctl0::RUN;
			stream->space.store(regs::stream::CTL0, ctl0);
		}
	}

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_stream_queue_data(UhdaStream* stream, const void* data, uint32_t* size) {
	if (!stream->output) {
		return UHDA_STATUS_UNSUPPORTED;
	}

	stream->queue_data(data, size);
	return UHDA_STATUS_SUCCESS;
}