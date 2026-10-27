#pragma once
#include <stdint.h>
#include <exception>
extern "C" void mag_movie_picture_begin(void);
extern "C" void mag_movie_picture_end(uint32_t object,int completed);
extern "C" void mag_movie_picture_command(uint32_t data_ea,uint32_t descriptor,uint32_t offset);
class MagMoviePictureScope {
    uint32_t object; int exceptions;
public:
    explicit MagMoviePictureScope(uint32_t ea):object(ea),exceptions(std::uncaught_exceptions()) {mag_movie_picture_begin();}
    ~MagMoviePictureScope(){mag_movie_picture_end(object,std::uncaught_exceptions()==exceptions);}
    MagMoviePictureScope(const MagMoviePictureScope&)=delete;
};
