use crate::error::Error;
use time;
use ffmpeg_next as ffmpeg;
use ffmpeg::{format, log, media, Rational, encoder, codec, util, Packet};

const AVERROR_BAD_REQUEST: i32 = 22;

pub fn prepare(){
    ffmpeg::init().expect("Failed to init ffmpeg");
    log::set_level(log::Level::Warning);
}

enum InputStream {
    Valid{
        mapping: u32,
        time_base: Rational,
        parameter: codec::Parameters,
    },
    Invalid,
}

struct Input<'a> {
    streams: Vec<InputStream>,
    count_valid_streams: u32,
    metadata: util::dictionary::Owned<'a>,
    // context: format::context::Input,
}

impl Input<'_> {
    fn new(path: &String) -> Result<(Self, format::context::Input), Error> {
        let context = match format::input(&path) {
            Ok(r) => r,
            Err(_) => return Err(Error::FailedToConnect)
        };
        let mut input = Self {
            streams: vec![],
            count_valid_streams: 0,
            metadata: context.metadata().to_owned(),
            // context,
        };
        for stream in context.streams() {
            input.streams.push(
                match stream.parameters().medium() {
                    media::Type::Audio | media::Type::Video | media::Type::Subtitle => InputStream::Valid {
                        mapping: {
                            let mapping = input.count_valid_streams;
                            input.count_valid_streams += 1;
                            mapping
                        },
                        time_base: stream.time_base(),
                        parameter: stream.parameters(),
                    },
                    _ => InputStream::Invalid,
                }
            )
        }
        Ok((input, context))
    }
}

struct Output {
    name: String,
    context: format::context::Output,
    count_streams: u32,
}

impl Output {
    fn new(name: String, input: &Input) -> Output {
        match crate::storage::ensure_parent_folder(&name) {
            Ok(_) => (),
            Err(e) => {
                println!("Failed to ensure parent folder for {} exists with error {}", name, e);
                panic!();
            }
        }
        let none_codec = encoder::find(codec::Id::None);
        let mut output = Output {
            context: format::output(&name).expect("Failed to open file"),
            name,
            count_streams: 0,
        };
        for in_stream in input.streams.iter() {
            if let InputStream::Valid { mapping: _, time_base: _, parameter } = in_stream {
                let mut out_stream = output.context.add_stream(none_codec).expect("Failed to add stream");
                out_stream.set_parameters(parameter.clone());
                unsafe {
                    (*out_stream.parameters().as_mut_ptr()).codec_tag = 0;
                }
                output.count_streams += 1;
            }
        }
        output.context.set_metadata(input.metadata.clone());
        output.context.write_header().expect("Failed to write header");
        println!("Opened new output {}", output.name);
        output
    }
    fn write_packet(&mut self, packet: &Packet) -> Result<(), Error> {
        if let Err(e) = packet.write_interleaved(&mut self.context) {
            println!("Error when writing packet: {:?}", e);
            if let ffmpeg::Error::Other {errno} = e {
                if errno != AVERROR_BAD_REQUEST {
                    return Err(Error::BrokenMux)
                }
            } else {
                return Err(Error::BrokenMux)
            }
        }
        Ok(())
    }
    fn close(mut self) {
        self.context.write_trailer().expect("Failed to close output");
        println!("Closed output {}", self.name);
    }
}


fn get_time(offset: &time::UtcOffset) -> time::OffsetDateTime {
    time::OffsetDateTime::now_utc().replace_offset(*offset)
}

fn get_next_time(time_now: &time::OffsetDateTime) -> time::OffsetDateTime {
    let minute = (time_now.minute() + 11) / 10 * 10;
    let mut time_next = time_now.clone();
    if minute >= 60 {
        time_next = time_next.replace_minute(0).expect("Failed to replace minute");
        time_next += time::Duration::HOUR;
    } else {
        time_next = time_next.replace_minute(minute).expect("Failed to replace minute");
    }
    time_next.replace_second(0).expect("Failed to replace second")
}

const EXTENSION: &str = ".mkv";

fn get_name(time: &time::OffsetDateTime, camera: &crate::camera::Camera, cameras_meta: &crate::camera::CamerasMetadata) -> String {
    format!("{}/{}_{}{}", cameras_meta.folder, time.format(&cameras_meta.time_formatter).expect("Failed to format time"), camera.name, EXTENSION)
}

pub(crate) fn mux_segmented(camera: &crate::camera::Camera, cameras_meta: &crate::camera::CamerasMetadata) -> Result<(), Error>{
    println!("Muxing from {}", camera.url);
    const TIME_STOP_LAG: time::Duration = time::Duration::seconds(5);
    let (input, mut input_context) = match Input::new(&camera.url) {
        Ok((input, input_context)) => (input, input_context),
        Err(e) => return Err(e),
    };
    let mut time_now = get_time(&cameras_meta.offset);
    let mut time_next = get_next_time(&time_now);
    let mut time_stop = time_next + TIME_STOP_LAG;
    println!("Camera {} started, now: {}, next: {}, stop: {}", camera.name, time_now, time_next, time_stop);
    let mut output_this = Output::new(get_name(&time_now, camera, cameras_meta), &input);
    let mut output_last: Option<Output> = None;
    for (stream, mut packet) in input_context.packets() {
        time_now = get_time(&cameras_meta.offset);
        if time_now >= time_next {
            if let Some(output) = output_last {
                output.close();
            }
            output_last = Some(output_this);
            output_this = Output::new(get_name(&time_now, camera, cameras_meta), &input);
            time_next = get_next_time(&time_now);
        }
        if time_now >= time_stop {
            if let Some(output) = output_last {
                output.close();
                output_last = None;
            }
            time_stop = time_next + TIME_STOP_LAG;
        }
        match input.streams.get(stream.index()).expect("Failed to get input stream info") {
            InputStream::Invalid => continue,
            InputStream::Valid { mapping, time_base, parameter: _ } => {
                let stream = output_this.context.stream(*mapping as _).expect("Failed to get stream from output");
                packet.rescale_ts(*time_base, stream.time_base());
                packet.set_position(-1);
                packet.set_stream(*mapping as _);
                if let Err(e) = output_this.write_packet(&packet) {
                    return Err(e);
                }
                if let Some(output_last) = &mut output_last {
                    if let Err(e) = output_last.write_packet(&packet) {
                        return Err(e);
                    }
                }
            }
        }
    }
    Ok(())
} 